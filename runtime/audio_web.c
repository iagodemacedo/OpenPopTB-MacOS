// Browser audio output (POPTB_WEB): a thread runs the DirectSound mixer (dsound_mix) and writes into a
// ring buffer in shared memory; an AudioWorklet on the page (web/audio-worklet.js) plays it.
//
// The mixer must run at real-time pace, because the game paces FMVs and QMixer's streaming on the
// buffers' play positions. With output running, the pace comes from the worklet: the ring is kept
// TARGET frames full. Before the page may play sound (browsers wait for a click or key press) or while
// the audio context is suspended, the mixer follows the clock instead and its output is discarded,
// like SDL's dummy driver.
#ifdef POPTB_WEB
#include "rt.h"
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>

void dsound_mix(float *mix, int frames);

#define RING_N 8192u    // frames (power of two), ~186 ms at 44.1 kHz
#define TARGET 3072u    // frames kept queued, ~70 ms of latency
#define CHUNK 256u      // frames mixed at a time
// [0] frames written (this thread), [1] frames read (worklet), [2] output running (page),
// [3] underruns (worklet); then RING_N stereo float32 frames
static struct { volatile int32_t hdr[4]; float data[RING_N * 2]; } ring __attribute__((aligned(16)));
static int out_rate;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void *mixer_main(void *arg) {
    (void)arg;
    static float buf[CHUNK * 2];
    double t_prev = now_s(), owed = 0;
    for (;;) {
        if (__atomic_load_n(&ring.hdr[2], __ATOMIC_ACQUIRE)) {
            uint32_t w = (uint32_t)ring.hdr[0], r = (uint32_t)__atomic_load_n(&ring.hdr[1], __ATOMIC_ACQUIRE);
            if (w - r + CHUNK <= TARGET) {
                dsound_mix(buf, CHUNK);
                for (uint32_t k = 0; k < CHUNK; k++) {
                    uint32_t j = ((w + k) & (RING_N - 1)) * 2;
                    ring.data[j] = buf[2 * k]; ring.data[j + 1] = buf[2 * k + 1];
                }
                __atomic_store_n(&ring.hdr[0], (int32_t)(w + CHUNK), __ATOMIC_RELEASE);
                continue;
            }
            usleep(2000);
            t_prev = now_s(); owed = 0;
        } else {
            // no output yet: advance at real-time pace and discard
            double t = now_s();
            owed += (t - t_prev) * out_rate; t_prev = t;
            if (owed > out_rate) owed = out_rate;   // after a long stall, don't race to catch up
            while (owed >= CHUNK) { dsound_mix(buf, CHUNK); owed -= CHUNK; }
            usleep(5000);
        }
    }
    return NULL;
}

int web_audio_start(int rate) {
    out_rate = rate;
    pthread_t th;
    if (pthread_create(&th, NULL, mixer_main, NULL)) { WARN("audio: cannot start the mixer thread"); return 0; }
    pthread_detach(th);
    // hands the ring to the page (web/main.js), which connects it to an AudioWorklet once sound is allowed
    MAIN_THREAD_ASYNC_EM_ASM({
        if (Module.poptbAudioStart) Module.poptbAudioStart(HEAPU8.buffer, $0, $1, $2);
    }, &ring, RING_N, rate);
    LOG("audio: browser output, %d Hz, %u frames queued", rate, TARGET);
    return 1;
}
#endif
