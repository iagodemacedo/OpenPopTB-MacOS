#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>

#include "assets.h"
#include "level.h"

typedef struct {
    float x, z;      // focus point in cells (wraps at 128)
    float yaw;       // radians
    float pitch;     // radians, angle below horizon
    float dist;      // cells from focus
} Camera;

typedef struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *terrain;  // RGBA copy of the indexed terrain texture
    SDL_Texture *sky;
    Level *level;
    Camera cam;
    SDL_Vertex *verts;
    int *order;
    float *depth;
} Renderer;

bool render_init(Renderer *r, const char *title, int w, int h);
void render_set_terrain(Renderer *r, Level *lv, const Theme *th, const uint8_t *indexed_tex);
void render_update_camera(Renderer *r, float dt, const bool *keys);
void render_frame(Renderer *r);  // draws; caller presents
void render_shutdown(Renderer *r);
