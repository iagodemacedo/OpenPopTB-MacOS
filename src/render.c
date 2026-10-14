#include "render.h"
#include "terrain.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Visible window of cells around the focus point. Placeholder tuning until the
// original projection (curvature + scale constants) is transcribed.
#define VIEW_R 40
#define VIEW_N (VIEW_R * 2)
#define HEIGHT_SCALE (1.0f / 320.0f) // height units per cell
#define CURVATURE 0.006f             // drop per cell^2 from the focus, fakes the globe

bool render_init(Renderer *r, const char *title, int w, int h) {
    memset(r, 0, sizeof *r);
    if (!SDL_CreateWindowAndRenderer(title, w, h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &r->win, &r->ren)) {
        SDL_Log("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(r->ren, 1);
    r->verts = malloc(sizeof(SDL_Vertex) * VIEW_N * VIEW_N * 6);
    r->order = malloc(sizeof(int) * VIEW_N * VIEW_N);
    r->depth = malloc(sizeof(float) * VIEW_N * VIEW_N);
    r->cam = (Camera){ .x = 64, .z = 64, .yaw = 0, .pitch = 0.62f, .dist = 42 };
    return true;
}

static SDL_Texture *upload_indexed(SDL_Renderer *ren, const PalEntry *pal, const uint8_t *src, int w, int h) {
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return NULL;
    uint32_t *rgba = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        PalEntry p = pal[src[i]];
        rgba[i] = 0xff000000u | ((uint32_t)p.b << 16) | ((uint32_t)p.g << 8) | p.r;
    }
    SDL_UpdateTexture(t, NULL, rgba, w * 4);
    free(rgba);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    return t;
}

void render_set_terrain(Renderer *r, Level *lv, const Theme *th, const uint8_t *tex) {
    if (r->terrain) SDL_DestroyTexture(r->terrain);
    if (r->sky) SDL_DestroyTexture(r->sky);
    r->level = lv;
    r->terrain = upload_indexed(r->ren, th->pal, tex, TERRAIN_TEX_W, TERRAIN_TEX_H);
    r->sky = upload_indexed(r->ren, th->pal, th->sky, 640, 480);
    r->cam.yaw = (float)lv->start_angle * (2.0f * (float)M_PI / 256.0f);
}

void render_update_camera(Renderer *r, float dt, const bool *keys) {
    Camera *c = &r->cam;
    float speed = 18.0f * dt, fx = sinf(c->yaw), fz = cosf(c->yaw);
    if (keys[SDL_SCANCODE_UP])    { c->x += fx * speed; c->z += fz * speed; }
    if (keys[SDL_SCANCODE_DOWN])  { c->x -= fx * speed; c->z -= fz * speed; }
    if (keys[SDL_SCANCODE_LEFT])  { c->x -= fz * speed; c->z += fx * speed; }
    if (keys[SDL_SCANCODE_RIGHT]) { c->x += fz * speed; c->z -= fx * speed; }
    if (keys[SDL_SCANCODE_Q]) c->yaw -= 1.5f * dt;
    if (keys[SDL_SCANCODE_E]) c->yaw += 1.5f * dt;
    if (keys[SDL_SCANCODE_W]) c->dist = fmaxf(8, c->dist - 20 * dt);
    if (keys[SDL_SCANCODE_S]) c->dist = fminf(70, c->dist + 20 * dt);
    c->x = fmodf(c->x + MAP_W, MAP_W);
    c->z = fmodf(c->z + MAP_H, MAP_H);
}

typedef struct { float x, y, depth; bool ok; } Proj;

static Proj project(const Renderer *r, float wx, float wy, float wz, float sw, float sh) {
    const Camera *c = &r->cam;
    // Relative to focus, rotated so the camera looks along +z.
    float cy = cosf(c->yaw), sy = sinf(c->yaw);
    float rx = wx * cy - wz * sy;
    float rz = wx * sy + wz * cy;
    wy -= (wx * wx + wz * wz) * CURVATURE;
    // Pitch around the focus, then push back by dist.
    float cp = cosf(c->pitch), sp = sinf(c->pitch);
    float vy = wy * cp + rz * sp;
    float vz = rz * cp - wy * sp + c->dist;
    Proj p = { 0, 0, vz, vz > 0.5f };
    float f = sh * 1.1f;
    p.x = sw * 0.5f + rx * f / vz;
    p.y = sh * 0.45f - vy * f / vz;
    return p;
}

static int cmp_depth_desc(void *ud, const void *a, const void *b) {
    const float *d = ud;
    float da = d[*(const int *)a], db = d[*(const int *)b];
    return da < db ? 1 : da > db ? -1 : 0;
}

void render_frame(Renderer *r) {
    int iw, ih;
    SDL_GetRenderOutputSize(r->ren, &iw, &ih);
    float sw = (float)iw, sh = (float)ih;
    SDL_SetRenderDrawColor(r->ren, 0, 0, 0, 255);
    SDL_RenderClear(r->ren);
    if (r->sky) SDL_RenderTexture(r->ren, r->sky, NULL, NULL);
    if (!r->level || !r->terrain) return;

    const Camera *c = &r->cam;
    int bx = (int)floorf(c->x), bz = (int)floorf(c->z);
    float fx = c->x - bx, fz = c->z - bz;
    int nq = 0;
    SDL_Vertex *v = r->verts;
    const float inv = 1.0f / TERRAIN_TEX_W;

    for (int dz = -VIEW_R; dz < VIEW_R; dz++) {
        for (int dx = -VIEW_R; dx < VIEW_R; dx++) {
            int cx = (bx + dx) & (MAP_W - 1), cz = (bz + dz) & (MAP_H - 1);
            static const int corner[4][2] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
            Proj p[4];
            bool ok = true;
            float dsum = 0;
            for (int k = 0; k < 4; k++) {
                float h = level_cell(r->level, cx + corner[k][0], cz + corner[k][1])->height * HEIGHT_SCALE;
                p[k] = project(r, dx + corner[k][0] - fx, h, dz + corner[k][1] - fz, sw, sh);
                ok &= p[k].ok;
                dsum += p[k].depth;
            }
            if (!ok) continue;
            float u0 = cx * 8 * inv, v0 = cz * 8 * inv, u1 = (cx + 1) * 8 * inv, v1 = (cz + 1) * 8 * inv;
            float uv[4][2] = { {u0, v0}, {u1, v0}, {u1, v1}, {u0, v1} };
            // Diagonal choice mirrors cell flag 0x1 from FUN_004e8450.
            static const int split_a[6] = { 0, 1, 2, 0, 2, 3 }, split_b[6] = { 0, 1, 3, 1, 2, 3 };
            const int *tri = (level_cell(r->level, cx, cz)->flags & 1) ? split_a : split_b;
            SDL_Vertex *q = v + nq * 6;
            for (int k = 0; k < 6; k++) {
                int i = tri[k];
                q[k].position = (SDL_FPoint){ p[i].x, p[i].y };
                q[k].color = (SDL_FColor){ 1, 1, 1, 1 };
                q[k].tex_coord = (SDL_FPoint){ uv[i][0], uv[i][1] };
            }
            r->depth[nq] = dsum * 0.25f;
            r->order[nq] = nq;
            nq++;
        }
    }
    // No depth buffer in SDL_RenderGeometry: painter's algorithm, far to near.
    SDL_qsort_r(r->order, (size_t)nq, sizeof(int), cmp_depth_desc, r->depth);
    static SDL_Vertex sorted[VIEW_N * VIEW_N * 6];
    for (int i = 0; i < nq; i++) memcpy(&sorted[i * 6], &v[r->order[i] * 6], sizeof(SDL_Vertex) * 6);
    SDL_RenderGeometry(r->ren, r->terrain, sorted, nq * 6, NULL, 0);

    SDL_SetRenderDrawColor(r->ren, 255, 255, 255, 255);
    SDL_RenderDebugTextFormat(r->ren, 8, 8, "%s  cam %.1f,%.1f  [arrows move, Q/E rotate, W/S zoom, PgUp/PgDn level]",
                              r->level->name, c->x, c->z);
}

void render_shutdown(Renderer *r) {
    if (r->terrain) SDL_DestroyTexture(r->terrain);
    if (r->sky) SDL_DestroyTexture(r->sky);
    free(r->verts);
    free(r->order);
    free(r->depth);
    SDL_DestroyRenderer(r->ren);
    SDL_DestroyWindow(r->win);
}
