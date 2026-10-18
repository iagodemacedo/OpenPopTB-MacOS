// Software DirectSound: buffers in guest memory, mixed into an SDL3 audio stream.
#include "rt.h"
#include <SDL3/SDL.h>
#include <sys/time.h>
#include <pthread.h>
#include "handles.h"
#include "input.h"
#include <unistd.h>

#define OUT_RATE 44100
#define MAXBUF 512

typedef struct Buf {
    uint32_t obj;
    uint32_t mem, size;          // guest memory
    int rate, chans, bits, align;
    int primary;
    volatile int playing, looping;
    double pos;                  // read position in frames (fractional)
    int vol, pan;                // hundredths of a dB
    uint32_t freq;               // 0 = original
    float gl, gr;                // precomputed gains
    int has3d;                   // DSBCAPS_CTRL3D
    float pos3[3], vel3[3], mind, maxd;
    uint32_t mode3, obj3d;
} Buf;

// global 3D listener (DirectSound has one per device)
static struct { float pos[3], vel[3], front[3], top[3], dist, roll, dopp; uint32_t obj; } L = {
    {0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 1, 1, 1, 0};
static uint32_t vt_l3d, vt_b3d;

static Buf *bufs[MAXBUF];
static pthread_mutex_t mix_mu = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioStream *stream;
static uint32_t vt_ds, vt_buf;
static int audio_ok = -1;

static void gains(Buf *b) {
    double v = b->vol <= -10000 ? 0 : pow(10.0, b->vol / 2000.0);
    double l = 1, r = 1;
    if (b->pan < 0) r = pow(10.0, b->pan / 2000.0);
    else if (b->pan > 0) l = pow(10.0, -b->pan / 2000.0);
    if (b->has3d && b->obj3d && b->mode3 != 2) {
        // inverse-distance attenuation (DirectSound model) and pan from the projection onto the "right" axis
        float rel[3];
        for (int i = 0; i < 3; i++) rel[i] = b->mode3 == 1 ? b->pos3[i] : b->pos3[i] - L.pos[i];
        double d = sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]) * (L.dist > 0 ? L.dist : 1);
        double mn = b->mind > 0 ? b->mind : 1, mx = b->maxd > mn ? b->maxd : 1e9;
        if (d > mx) d = mx;
        double att = d <= mn ? 1.0 : mn / (mn + L.roll * (d - mn));
        float *f = L.front, *t = L.top;
        double rx = t[1] * f[2] - t[2] * f[1], ry = t[2] * f[0] - t[0] * f[2], rz = t[0] * f[1] - t[1] * f[0];
        double rn = sqrt(rx * rx + ry * ry + rz * rz); if (rn < 1e-9) rn = 1;
        double dn = sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
        double px = dn > 1e-6 ? (rel[0] * rx + rel[1] * ry + rel[2] * rz) / (rn * dn) : 0;
        if (b->mode3 == 1) px = dn > 1e-6 ? rel[0] / dn : 0;
        double a = (px + 1) * M_PI / 4;   // constant-power pan
        l *= cos(a) * M_SQRT2; r *= sin(a) * M_SQRT2;
        v *= att;
    }
    b->gl = (float)(v * l); b->gr = (float)(v * r);
}

// ---- waveOut: WAVEHDR queue played by the same mixer ----
typedef struct WaveDev {
    int open, paused;
    int rate, chans, bits, align;
    uint32_t cbtype, cb, inst, hwo;
    uint32_t q[256]; int qh, qt;     // queued guest WAVEHDRs
    double pos;                      // frames consumed from the current header
    uint64_t played;                 // bytes played (waveOutGetPosition)
} WaveDev;
static WaveDev wdev;
// WOM_DONE notifications delivered by a thread holding the GIL (the audio callback can't call the guest)
static uint32_t done_q[1024]; static volatile int done_h, done_t;
static pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;

static void wave_mix(float *mix, int frames) {
    WaveDev *w = &wdev;
    if (!w->open || w->paused || !w->align) return;
    double step = (double)w->rate / OUT_RATE;
    int k = 0;
    while (k < frames && w->qh != w->qt) {
        uint32_t hdr = w->q[w->qh];
        uint32_t data = rd32(hdr), len = rd32(hdr + 4);
        uint32_t nfr = len / (uint32_t)w->align;
        const uint8_t *m = PTR(data);
        while (k < frames) {
            uint32_t f = (uint32_t)w->pos;
            if (f >= nfr) break;
            const uint8_t *q = m + f * w->align;
            float l, r;
            if (w->bits == 16) { l = (int16_t)(q[0] | q[1] << 8) / 32768.0f; r = w->chans == 2 ? (int16_t)(q[2] | q[3] << 8) / 32768.0f : l; }
            else { l = (q[0] - 128) / 128.0f; r = w->chans == 2 ? (q[1] - 128) / 128.0f : l; }
            mix[2 * k] += l; mix[2 * k + 1] += r;
            w->pos += step; k++;
        }
        if ((uint32_t)w->pos >= nfr) {
            w->played += len;
            w->pos -= nfr;
            if (w->pos < 0) w->pos = 0;
            wr32(hdr + 16, (rd32(hdr + 16) & ~0x10u) | 1u);   // DONE, dequeued
            w->qh = (w->qh + 1) % 256;
            int n = (done_t + 1) % 1024;
            if (n != done_h) { done_q[done_t] = hdr; done_t = n; }
        }
    }
    if (done_h != done_t) pthread_cond_signal(&done_cv);
}

static void *wave_notify_main(void *arg) {
    (void)arg;
    CPU *R = NULL;
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    for (;;) {
        pthread_mutex_lock(&m);
        while (done_h == done_t) {
            struct timespec ts = {0, 0};
            struct timeval tv; gettimeofday(&tv, NULL);
            ts.tv_sec = tv.tv_sec; ts.tv_nsec = tv.tv_usec * 1000 + 5000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&done_cv, &m, &ts);
        }
        pthread_mutex_unlock(&m);
        gil_lock();
        if (!R) R = cpu_new_thread();
        while (done_h != done_t) {
            uint32_t hdr = done_q[done_h]; done_h = (done_h + 1) % 1024;
            WaveDev *w = &wdev;
            switch (w->cbtype) {
            case 0x30000: guest_call(R, w->cb, 5, w->hwo, 0x3BD, w->inst, hdr, 0); break;   // CALLBACK_FUNCTION
            case 0x10000: msgq_post(w->cb, 0x3BD, w->hwo, hdr); break;                       // CALLBACK_WINDOW
            case 0x50000: { Handle *x = handle_get(w->cb); if (x) { x->signaled = 1; handles_signal(); } break; }
            default: break;
            }
        }
        gil_unlock();
    }
    return NULL;
}

static void SDLCALL audio_cb(void *ud, SDL_AudioStream *s, int additional, int total) {
    (void)ud; (void)total;
    int frames = additional / 8;
    if (frames <= 0) return;
    static float *mix; static int cap;
    if (frames > cap) { free(mix); mix = malloc((size_t)frames * 8); cap = frames; }
    memset(mix, 0, (size_t)frames * 8);
    pthread_mutex_lock(&mix_mu);
    for (int i = 0; i < MAXBUF; i++) {
        Buf *b = bufs[i];
        if (!b || !b->playing || !b->size || !b->align) continue;
        if (b->has3d) gains(b);
        uint32_t nfr = b->size / (uint32_t)b->align;
        double step = (double)(b->freq ? b->freq : (uint32_t)b->rate) / OUT_RATE;
        const uint8_t *m = PTR(b->mem);
        double p = b->pos;
        for (int k = 0; k < frames; k++) {
            uint32_t f = (uint32_t)p;
            if (f >= nfr) {
                if (b->looping) { p -= nfr; f = (uint32_t)p; if (f >= nfr) { p = 0; f = 0; } }
                else { b->playing = 0; break; }
            }
            const uint8_t *q = m + f * b->align;
            float l, r;
            if (b->bits == 16) {
                l = (int16_t)(q[0] | q[1] << 8) / 32768.0f;
                r = b->chans == 2 ? (int16_t)(q[2] | q[3] << 8) / 32768.0f : l;
            } else {
                l = (q[0] - 128) / 128.0f;
                r = b->chans == 2 ? (q[1] - 128) / 128.0f : l;
            }
            mix[2 * k] += l * b->gl;
            mix[2 * k + 1] += r * b->gr;
            p += step;
        }
        b->pos = p;
    }
    wave_mix(mix, frames);
    pthread_mutex_unlock(&mix_mu);
    for (int k = 0; k < frames * 2; k++) { float v = mix[k]; mix[k] = v > 1 ? 1 : v < -1 ? -1 : v; }
    SDL_PutAudioStreamData(s, mix, frames * 8);
    // diagnostic: POPTB_WAV=<file> records the mixer output (float32 stereo 44.1 kHz, headerless)
    static FILE *wav; static int wav_init;
    if (!wav_init) { wav_init = 1; if (getenv("POPTB_WAV")) wav = fopen(getenv("POPTB_WAV"), "wb"); }
    if (wav) { fwrite(mix, 8, (size_t)frames, wav); fflush(wav); }
}

int audio_init(void) {
    if (audio_ok >= 0) return audio_ok;
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, OUT_RATE};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
    if (!stream) { WARN("audio unavailable: %s", SDL_GetError()); audio_ok = 0; return 0; }
    SDL_ResumeAudioStreamDevice(stream);
    audio_ok = 1;
    return 1;
}

static Buf *B(uint32_t o) { return com_native(o); }

static void buf_release(CPU *R) {
    uint32_t o = ARG(0); uint32_t c = rd32(o + 4); if (c) c--; wr32(o + 4, c);
    if (!c) {
        Buf *b = B(o);
        pthread_mutex_lock(&mix_mu);
        for (int i = 0; i < MAXBUF; i++) if (bufs[i] == b) bufs[i] = NULL;
        pthread_mutex_unlock(&mix_mu);
        if (b) { b->playing = 0; }
    }
    R->eax = c; R->esp += 8;
}
static void ds_release(CPU *R) { uint32_t o = ARG(0); uint32_t c = rd32(o + 4); if (c) c--; wr32(o + 4, c); R->eax = c; R->esp += 8; }

static void set_format(Buf *b, uint32_t wf) {
    if (!wf) return;
    b->chans = (int)rd16(wf + 2); b->rate = (int)rd32(wf + 4); b->bits = (int)rd16(wf + 14);
    b->align = (int)rd16(wf + 12);
    if (!b->align) b->align = b->chans * b->bits / 8;
}

// ---------------- IDirectSound ----------------
COMFN(ds_QueryInterface, 2) { LOG("IDirectSound::QueryInterface %08x", rd32(ARG(1))); wr32(ARG(2), 0); return 0x80004002; }
COMFN(ds_CreateSoundBuffer, 3) {
    uint32_t d = ARG(1), out = ARG(2);
    uint32_t fl = rd32(d + 4), bytes = rd32(d + 8), wf = rd32(d + 16);
    Buf *b = calloc(1, sizeof *b);
    b->primary = (fl & 1) != 0;
    b->has3d = (fl & 0x10) != 0;
    b->mind = 1; b->maxd = 1e9;
    b->rate = 22050; b->chans = 2; b->bits = 16; b->align = 4;
    set_format(b, wf);
    if (b->primary) bytes = (uint32_t)(b->rate * b->align / 4);   // ~250 ms
    b->size = bytes;
    b->mem = bytes ? gmalloc(bytes) : 0;
    if (b->bits == 8 && b->mem) memset(PTR(b->mem), 0x80, bytes);
    gains(b);
    static const char *dummy;
    (void)dummy;
    b->obj = com_new("IDirectSoundBuffer", vt_buf, 0, b);
    pthread_mutex_lock(&mix_mu);
    for (int i = 0; i < MAXBUF; i++) if (!bufs[i]) { bufs[i] = b; break; }
    pthread_mutex_unlock(&mix_mu);
    LOG("DS CreateSoundBuffer flags=%x bytes=%u %dHz %dch %dbit%s", fl, bytes, b->rate, b->chans, b->bits, b->primary ? " (primary)" : "");
    wr32(out, b->obj);
    return 0;
}
COMFN(ds_GetCaps, 1) {
    uint32_t c = ARG(1); uint32_t sz = rd32(c);
    if (sz < 8 || sz > 256) sz = 96;
    memset(PTR(c), 0, sz); wr32(c, sz);
    wr32(c + 4, 0x00000f1f);   // supports mono/stereo 8/16-bit in software
    wr32(c + 8, 100); wr32(c + 12, 100000);
    return 0;
}
COMFN(ds_DuplicateSoundBuffer, 2) {
    Buf *o = B(ARG(1));
    Buf *b = calloc(1, sizeof *b);
    *b = *o; b->playing = 0; b->pos = 0;
    b->obj = com_new("IDirectSoundBuffer", vt_buf, 0, b);
    pthread_mutex_lock(&mix_mu);
    for (int i = 0; i < MAXBUF; i++) if (!bufs[i]) { bufs[i] = b; break; }
    pthread_mutex_unlock(&mix_mu);
    wr32(ARG(2), b->obj);
    return 0;
}
COMFN(ds_SetCooperativeLevel, 2) { LOG("DS SetCooperativeLevel %u", ARG(2)); return 0; }
COMFN(ds_Compact, 0) { return 0; }
COMFN(ds_GetSpeakerConfig, 1) { wr32(ARG(1), 4); return 0; }   // DSSPEAKER_STEREO
COMFN(ds_SetSpeakerConfig, 1) { return 0; }
COMFN(ds_Initialize, 1) { return 0; }

// ---------------- IDirectSoundBuffer ----------------
COMFN(b_QueryInterface, 2) {
    Buf *b = B(THIS);
    uint32_t iid = rd32(ARG(1));
    if (iid == 0x279AFA85) { wr32(ARG(2), THIS); wr32(THIS + 4, rd32(THIS + 4) + 1); return 0; }   // IDirectSoundBuffer
    if (iid == 0x279AFA84 && b->primary) {                                                          // IDirectSound3DListener
        if (!L.obj) L.obj = com_new("IDirectSound3DListener", vt_l3d, 0, NULL);
        wr32(ARG(2), L.obj); return 0;
    }
    if (iid == 0x279AFA86 && !b->primary) {                                                         // IDirectSound3DBuffer
        if (!b->obj3d) { b->obj3d = com_new("IDirectSound3DBuffer", vt_b3d, 0, b); b->has3d = 1; }
        wr32(ARG(2), b->obj3d); return 0;
    }
    LOG("IDirectSoundBuffer::QueryInterface %08x not supported", iid);
    wr32(ARG(2), 0);
    return 0x80004002;
}

// ---------------- IDirectSound3DListener / IDirectSound3DBuffer ----------------
static float F(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static uint32_t U32(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static void wrv(uint32_t p, const float *v) { for (int i = 0; i < 3; i++) wr32(p + 4 * i, U32(v[i])); }
static void rdv(uint32_t p, float *v) { for (int i = 0; i < 3; i++) v[i] = F(rd32(p + 4 * i)); }
static void x3_addref(CPU *R) { R->eax = 1; R->esp += 8; }
COMFN(x3_QueryInterface, 2) { wr32(ARG(2), 0); return 0x80004002; }

COMFN(l_GetAllParameters, 1) {
    uint32_t p = ARG(1);
    wrv(p + 4, L.pos); wrv(p + 16, L.vel); wrv(p + 28, L.front); wrv(p + 40, L.top);
    wr32(p + 52, U32(L.dist)); wr32(p + 56, U32(L.roll)); wr32(p + 60, U32(L.dopp));
    return 0;
}
COMFN(l_GetDistanceFactor, 1) { wr32(ARG(1), U32(L.dist)); return 0; }
COMFN(l_GetDopplerFactor, 1) { wr32(ARG(1), U32(L.dopp)); return 0; }
COMFN(l_GetOrientation, 2) { wrv(ARG(1), L.front); wrv(ARG(2), L.top); return 0; }
COMFN(l_GetPosition, 1) { wrv(ARG(1), L.pos); return 0; }
COMFN(l_GetRolloffFactor, 1) { wr32(ARG(1), U32(L.roll)); return 0; }
COMFN(l_GetVelocity, 1) { wrv(ARG(1), L.vel); return 0; }
COMFN(l_SetAllParameters, 2) {
    uint32_t p = ARG(1);
    rdv(p + 4, L.pos); rdv(p + 16, L.vel); rdv(p + 28, L.front); rdv(p + 40, L.top);
    L.dist = F(rd32(p + 52)); L.roll = F(rd32(p + 56)); L.dopp = F(rd32(p + 60));
    return 0;
}
COMFN(l_SetDistanceFactor, 2) { L.dist = F(ARG(1)); return 0; }
COMFN(l_SetDopplerFactor, 2) { L.dopp = F(ARG(1)); return 0; }
COMFN(l_SetOrientation, 7) {
    for (int i = 0; i < 3; i++) { L.front[i] = F(ARG(1 + i)); L.top[i] = F(ARG(4 + i)); }
    return 0;
}
COMFN(l_SetPosition, 4) { for (int i = 0; i < 3; i++) L.pos[i] = F(ARG(1 + i)); return 0; }
COMFN(l_SetRolloffFactor, 2) { L.roll = F(ARG(1)); return 0; }
COMFN(l_SetVelocity, 4) { for (int i = 0; i < 3; i++) L.vel[i] = F(ARG(1 + i)); return 0; }
COMFN(l_CommitDeferredSettings, 0) { return 0; }

static Buf *B3(uint32_t o) { return com_native(o); }
COMFN(b3_GetAllParameters, 1) {
    Buf *b = B3(THIS); uint32_t p = ARG(1);
    wrv(p + 4, b->pos3); wrv(p + 16, b->vel3);
    wr32(p + 28, 360); wr32(p + 32, 360);
    float o[3] = {0, 0, 1}; wrv(p + 36, o);
    wr32(p + 48, 0); wr32(p + 52, U32(b->mind)); wr32(p + 56, U32(b->maxd)); wr32(p + 60, b->mode3);
    return 0;
}
COMFN(b3_GetConeAngles, 2) { if (ARG(1)) wr32(ARG(1), 360); if (ARG(2)) wr32(ARG(2), 360); return 0; }
COMFN(b3_GetConeOrientation, 1) { float o[3] = {0, 0, 1}; wrv(ARG(1), o); return 0; }
COMFN(b3_GetConeOutsideVolume, 1) { wr32(ARG(1), 0); return 0; }
COMFN(b3_GetMaxDistance, 1) { wr32(ARG(1), U32(B3(THIS)->maxd)); return 0; }
COMFN(b3_GetMinDistance, 1) { wr32(ARG(1), U32(B3(THIS)->mind)); return 0; }
COMFN(b3_GetMode, 1) { wr32(ARG(1), B3(THIS)->mode3); return 0; }
COMFN(b3_GetPosition, 1) { wrv(ARG(1), B3(THIS)->pos3); return 0; }
COMFN(b3_GetVelocity, 1) { wrv(ARG(1), B3(THIS)->vel3); return 0; }
COMFN(b3_SetAllParameters, 2) {
    Buf *b = B3(THIS); uint32_t p = ARG(1);
    rdv(p + 4, b->pos3); rdv(p + 16, b->vel3);
    b->mind = F(rd32(p + 52)); b->maxd = F(rd32(p + 56)); b->mode3 = rd32(p + 60);
    return 0;
}
COMFN(b3_SetConeAngles, 3) { return 0; }
COMFN(b3_SetConeOrientation, 4) { return 0; }
COMFN(b3_SetConeOutsideVolume, 2) { return 0; }
COMFN(b3_SetMaxDistance, 2) { B3(THIS)->maxd = F(ARG(1)); return 0; }
COMFN(b3_SetMinDistance, 2) { B3(THIS)->mind = F(ARG(1)); return 0; }
COMFN(b3_SetMode, 2) { B3(THIS)->mode3 = ARG(1); return 0; }
COMFN(b3_SetPosition, 4) { Buf *b = B3(THIS); for (int i = 0; i < 3; i++) b->pos3[i] = F(ARG(1 + i)); return 0; }
COMFN(b3_SetVelocity, 4) { Buf *b = B3(THIS); for (int i = 0; i < 3; i++) b->vel3[i] = F(ARG(1 + i)); return 0; }
COMFN(b_GetCaps, 1) {
    Buf *b = B(THIS); uint32_t c = ARG(1);
    wr32(c, 20); wr32(c + 4, b->primary ? 1 : 0); wr32(c + 8, b->size); wr32(c + 12, 0); wr32(c + 16, 0);
    return 0;
}
COMFN(b_GetCurrentPosition, 2) {
    Buf *b = B(THIS);
    uint32_t play = 0;
    if (b->size && b->align) play = ((uint32_t)b->pos * (uint32_t)b->align) % b->size;
    uint32_t lead = (uint32_t)(b->rate / 50) * (uint32_t)b->align;   // ~20 ms
    if (rt_verbose > 1) { static int n; if (n++ % 200 == 0) LOG("[%u] DS GetCurrentPosition %x play=%u playing=%d (#%d)", now_ms() % 100000, THIS, play, b->playing, n); }
    if (ARG(1)) wr32(ARG(1), play);
    if (ARG(2)) wr32(ARG(2), b->size ? (play + lead) % b->size : 0);
    return 0;
}
COMFN(b_GetFormat, 3) {
    Buf *b = B(THIS); uint32_t wf = ARG(1);
    if (wf) {
        wr16(wf, 1); wr16(wf + 2, b->chans); wr32(wf + 4, b->rate); wr32(wf + 8, b->rate * b->align);
        wr16(wf + 12, b->align); wr16(wf + 14, b->bits);
        if (ARG(2) >= 18) wr16(wf + 16, 0);
    }
    if (ARG(3)) wr32(ARG(3), 18);
    return 0;
}
COMFN(b_GetVolume, 1) { wr32(ARG(1), (uint32_t)B(THIS)->vol); return 0; }
COMFN(b_GetPan, 1) { wr32(ARG(1), (uint32_t)B(THIS)->pan); return 0; }
COMFN(b_GetFrequency, 1) { Buf *b = B(THIS); wr32(ARG(1), b->freq ? b->freq : (uint32_t)b->rate); return 0; }
COMFN(b_GetStatus, 1) { Buf *b = B(THIS); wr32(ARG(1), (b->playing ? 1 : 0) | (b->playing && b->looping ? 4 : 0)); return 0; }
COMFN(b_Initialize, 2) { return 0x88780082; }   // DSERR_ALREADYINITIALIZED
COMFN(b_Lock, 7) {
    Buf *b = B(THIS);
    if (rt_verbose > 1) { static int n; if (n++ % 50 == 0) LOG("[%u] DS Lock %x off=%u n=%u fl=%x (#%d)", now_ms() % 100000, THIS, ARG(1), ARG(2), ARG(7), n); }
    uint32_t off = ARG(1), n = ARG(2), fl = ARG(7);
    if (fl & 2) { off = 0; n = b->size; }
    else if (fl & 1) {
        uint32_t play = b->align ? ((uint32_t)b->pos * (uint32_t)b->align) % (b->size ? b->size : 1) : 0;
        off = (play + (uint32_t)(b->rate / 50) * (uint32_t)b->align) % (b->size ? b->size : 1);
    }
    if (!b->size) return 0x80070057;
    off %= b->size;
    if (n > b->size) n = b->size;
    uint32_t n1 = n, n2 = 0;
    if (off + n > b->size) { n1 = b->size - off; n2 = n - n1; }
    wr32(ARG(3), b->mem + off); wr32(ARG(4), n1);
    if (ARG(5)) wr32(ARG(5), n2 ? b->mem : 0);
    if (ARG(6)) wr32(ARG(6), n2);
    return 0;
}
COMFN(b_Play, 3) {
    Buf *b = B(THIS);
    LOG("DS Play %x flags=%x", THIS, ARG(3));
    audio_init();
    b->looping = (ARG(3) & 1) != 0;
    b->playing = 1;
    return 0;
}
COMFN(b_SetCurrentPosition, 1) { Buf *b = B(THIS); if (b->align) b->pos = (double)(ARG(1) / (uint32_t)b->align); return 0; }
COMFN(b_SetFormat, 1) { Buf *b = B(THIS); set_format(b, ARG(1)); return 0; }
COMFN(b_SetVolume, 1) { Buf *b = B(THIS); b->vol = (int)ARG(1); gains(b); return 0; }
COMFN(b_SetPan, 1) { Buf *b = B(THIS); b->pan = (int)ARG(1); gains(b); return 0; }
COMFN(b_SetFrequency, 1) { B(THIS)->freq = ARG(1); return 0; }
COMFN(b_Stop, 0) { LOG("DS Stop %x", THIS); B(THIS)->playing = 0; return 0; }
COMFN(b_Unlock, 4) { return 0; }
COMFN(b_Restore, 0) { return 0; }

static void build(void) {
    static const char *dm[] = {"QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps", "DuplicateSoundBuffer",
        "SetCooperativeLevel", "Compact", "GetSpeakerConfig", "SetSpeakerConfig", "Initialize"};
    native_fn df[] = {ds_QueryInterface, com_addref, ds_release, ds_CreateSoundBuffer, ds_GetCaps, ds_DuplicateSoundBuffer,
        ds_SetCooperativeLevel, ds_Compact, ds_GetSpeakerConfig, ds_SetSpeakerConfig, ds_Initialize};
    vt_ds = com_vtbl("IDirectSound", dm, 11, df);
    static const char *bm[] = {"QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition", "GetFormat",
        "GetVolume", "GetPan", "GetFrequency", "GetStatus", "Initialize", "Lock", "Play", "SetCurrentPosition",
        "SetFormat", "SetVolume", "SetPan", "SetFrequency", "Stop", "Unlock", "Restore"};
    native_fn bf[] = {b_QueryInterface, com_addref, buf_release, b_GetCaps, b_GetCurrentPosition, b_GetFormat,
        b_GetVolume, b_GetPan, b_GetFrequency, b_GetStatus, b_Initialize, b_Lock, b_Play, b_SetCurrentPosition,
        b_SetFormat, b_SetVolume, b_SetPan, b_SetFrequency, b_Stop, b_Unlock, b_Restore};
    vt_buf = com_vtbl("IDirectSoundBuffer", bm, 21, bf);
    static const char *lm[] = {"QueryInterface", "AddRef", "Release", "GetAllParameters", "GetDistanceFactor",
        "GetDopplerFactor", "GetOrientation", "GetPosition", "GetRolloffFactor", "GetVelocity", "SetAllParameters",
        "SetDistanceFactor", "SetDopplerFactor", "SetOrientation", "SetPosition", "SetRolloffFactor", "SetVelocity",
        "CommitDeferredSettings"};
    native_fn lf[] = {x3_QueryInterface, x3_addref, x3_addref, l_GetAllParameters, l_GetDistanceFactor,
        l_GetDopplerFactor, l_GetOrientation, l_GetPosition, l_GetRolloffFactor, l_GetVelocity, l_SetAllParameters,
        l_SetDistanceFactor, l_SetDopplerFactor, l_SetOrientation, l_SetPosition, l_SetRolloffFactor, l_SetVelocity,
        l_CommitDeferredSettings};
    vt_l3d = com_vtbl("IDirectSound3DListener", lm, 18, lf);
    static const char *b3m[] = {"QueryInterface", "AddRef", "Release", "GetAllParameters", "GetConeAngles",
        "GetConeOrientation", "GetConeOutsideVolume", "GetMaxDistance", "GetMinDistance", "GetMode", "GetPosition",
        "GetVelocity", "SetAllParameters", "SetConeAngles", "SetConeOrientation", "SetConeOutsideVolume",
        "SetMaxDistance", "SetMinDistance", "SetMode", "SetPosition", "SetVelocity"};
    native_fn b3f[] = {x3_QueryInterface, x3_addref, x3_addref, b3_GetAllParameters, b3_GetConeAngles,
        b3_GetConeOrientation, b3_GetConeOutsideVolume, b3_GetMaxDistance, b3_GetMinDistance, b3_GetMode, b3_GetPosition,
        b3_GetVelocity, b3_SetAllParameters, b3_SetConeAngles, b3_SetConeOrientation, b3_SetConeOutsideVolume,
        b3_SetMaxDistance, b3_SetMinDistance, b3_SetMode, b3_SetPosition, b3_SetVelocity};
    vt_b3d = com_vtbl("IDirectSound3DBuffer", b3m, 21, b3f);
}

uint32_t dsound_create_object(void) {
    if (!vt_ds) build();
    if (!audio_init()) return 0;
    return com_new("IDirectSound", vt_ds, 0, NULL);
}

// DirectSoundCreate is ordinal 1 of dsound.dll
SHIM_AS(dsound_create, "#1", 3) {
    uint32_t o = dsound_create_object();
    if (!o) return 0x88780078;   // DSERR_NODRIVER
    wr32(ARG(1), o);
    LOG("DirectSoundCreate -> %x", o);
    return 0;
}
SHIM(DirectSoundCreate, 3) {
    uint32_t o = dsound_create_object();
    if (!o) return 0x88780078;
    wr32(ARG(1), o);
    return 0;
}

// ---------------- waveOut ----------------
SHIM(waveOutGetNumDevs, 0) { return 1; }
SHIM(waveOutGetDevCapsA, 3) {
    uint32_t c = ARG(1);
    memset(PTR(c), 0, 52);
    wr16(c, 1); wr16(c + 2, 1); wr32(c + 4, 0x0100);
    strcpy(gstr(c + 8), "SDL Audio");
    wr32(c + 40, 0xfff); wr16(c + 44, 2); wr32(c + 48, 0x2c);
    return 0;
}
SHIM(waveOutOpen, 6) {
    uint32_t phwo = ARG(0), wf = ARG(2), cb = ARG(3), inst = ARG(4), fl = ARG(5);
    if (rd16(wf) != 1) return 32;   // WAVERR_BADFORMAT
    if (fl & 1) return 0;           // WAVE_FORMAT_QUERY
    if (!audio_init()) return 2;
    static int started;
    if (!started) { started = 1; pthread_t t; pthread_create(&t, NULL, wave_notify_main, NULL); pthread_detach(t); }
    pthread_mutex_lock(&mix_mu);
    WaveDev *w = &wdev;
    memset(w, 0, sizeof *w);
    w->chans = (int)rd16(wf + 2); w->rate = (int)rd32(wf + 4); w->bits = (int)rd16(wf + 14); w->align = (int)rd16(wf + 12);
    w->cbtype = fl & 0x70000; w->cb = cb; w->inst = inst; w->hwo = 0xa0de;
    w->open = 1;
    pthread_mutex_unlock(&mix_mu);
    if (phwo) wr32(phwo, w->hwo);
    LOG("waveOutOpen %dHz %dch %dbit callback=%x type=%x", w->rate, w->chans, w->bits, cb, w->cbtype);
    if (w->cbtype == 0x30000) guest_call(R, cb, 5, w->hwo, 0x3BB, inst, 0, 0);    // WOM_OPEN
    else if (w->cbtype == 0x10000) msgq_post(cb, 0x3BB, w->hwo, 0);
    return 0;
}
SHIM(waveOutClose, 1) {
    pthread_mutex_lock(&mix_mu); wdev.open = 0; wdev.qh = wdev.qt = 0; pthread_mutex_unlock(&mix_mu);
    return 0;
}
SHIM(waveOutPrepareHeader, 3) { wr32(ARG(1) + 16, rd32(ARG(1) + 16) | 2); return 0; }
SHIM(waveOutUnprepareHeader, 3) { wr32(ARG(1) + 16, rd32(ARG(1) + 16) & ~2u); return 0; }
SHIM(waveOutWrite, 3) {
    uint32_t hdr = ARG(1);
    pthread_mutex_lock(&mix_mu);
    WaveDev *w = &wdev;
    int n = (w->qt + 1) % 256;
    if (n == w->qh) { pthread_mutex_unlock(&mix_mu); return 33; }   // WAVERR_STILLPLAYING
    wr32(hdr + 16, (rd32(hdr + 16) & ~1u) | 0x10);
    w->q[w->qt] = hdr; w->qt = n;
    pthread_mutex_unlock(&mix_mu);
    return 0;
}
SHIM(waveOutReset, 1) {
    pthread_mutex_lock(&mix_mu);
    WaveDev *w = &wdev;
    while (w->qh != w->qt) {
        uint32_t hdr = w->q[w->qh];
        wr32(hdr + 16, (rd32(hdr + 16) & ~0x10u) | 1u);
        w->qh = (w->qh + 1) % 256;
        int n = (done_t + 1) % 1024;
        if (n != done_h) { done_q[done_t] = hdr; done_t = n; }
    }
    w->pos = 0;
    pthread_mutex_unlock(&mix_mu);
    pthread_cond_signal(&done_cv);
    return 0;
}
SHIM(waveOutPause, 1) { wdev.paused = 1; return 0; }
SHIM(waveOutRestart, 1) { wdev.paused = 0; return 0; }
SHIM(waveOutGetPosition, 3) {
    uint32_t mt = ARG(1);
    uint32_t type = rd32(mt);
    uint64_t b = wdev.played + (uint64_t)wdev.pos * (uint64_t)wdev.align;
    if (type == 4) wr32(mt + 4, (uint32_t)b);                                   // TIME_BYTES
    else if (type == 2) wr32(mt + 4, (uint32_t)(b / (wdev.align ? wdev.align : 1)));  // TIME_SAMPLES
    else { wr32(mt, 4); wr32(mt + 4, (uint32_t)b); }
    return 0;
}
SHIM(waveOutSetVolume, 2) { return 0; }
SHIM(waveOutGetVolume, 2) { if (ARG(1)) wr32(ARG(1), 0xffffffff); return 0; }
