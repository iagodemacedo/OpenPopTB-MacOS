#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "fs.h"
#include "level.h"
#include "render.h"
#include "terrain.h"

static char theme_key(int idx) { return (char)(idx < 10 ? '0' + idx : 'a' + idx - 10); }

typedef struct {
    Level level;
    Theme theme;
    TerrainTables tables;
    uint8_t *terrain_tex;  // TERRAIN_TEX_W x TERRAIN_TEX_H indexed
} Game;

static int game_load_level(Game *g, int n) {
    if (!level_load(&g->level, n)) return 0;
    char key = theme_key(g->level.theme);
    theme_free(&g->theme);
    terrain_tables_free(&g->tables);
    // FUN_004503f0 falls back to theme 0 when the themed palette is missing.
    if (!theme_load(&g->theme, key) || !terrain_tables_load(&g->tables, key)) {
        key = '0';
        if (!theme_load(&g->theme, key) || !terrain_tables_load(&g->tables, key)) return 0;
    }
    terrain_build_texture(&g->tables, &g->level, g->terrain_tex);
    SDL_Log("level %d '%s' tribes=%d theme=%c", n, g->level.name, g->level.num_tribes, key);
    return 1;
}

static int dump_ppm(Game *g, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", TERRAIN_TEX_W, TERRAIN_TEX_H);
    for (int i = 0; i < TERRAIN_TEX_W * TERRAIN_TEX_H; i++) {
        PalEntry p = g->theme.pal[g->terrain_tex[i]];
        fputc(p.r, f); fputc(p.g, f); fputc(p.b, f);
    }
    fclose(f);
    return 1;
}

int main(int argc, char **argv) {
    const char *root = getenv("POPTB_DATA");
    int level_no = 1;
    const char *dump = NULL;
    const char *shot = NULL;  // render a few frames, save a BMP, exit (for automated checks)
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) level_no = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    }
    if (!root) {
        fprintf(stderr, "usage: OpenPopTB --data /path/to/populous [--level N] [--dump out.ppm]\n");
        return 2;
    }
    fs_set_root(root);

    static Game g;
    g.terrain_tex = malloc(TERRAIN_TEX_W * TERRAIN_TEX_H);
    if (!game_load_level(&g, level_no)) {
        fprintf(stderr, "failed to load level %d\n", level_no);
        return 1;
    }
    if (dump) return dump_ppm(&g, dump) ? 0 : 1;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    Renderer r;
    if (!render_init(&r, "OpenPopTB", 1280, 960)) return 1;
    render_set_terrain(&r, &g.level, &g.theme, g.terrain_tex);

    int running = 1, frames = 0;
    Uint64 last = SDL_GetTicksNS();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = 0;
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) running = 0;
                else if (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) {
                    int n = level_no + (k == SDLK_PAGEUP ? 1 : -1);
                    if (n >= 1 && n <= 25 && game_load_level(&g, n)) {
                        level_no = n;
                        render_set_terrain(&r, &g.level, &g.theme, g.terrain_tex);
                    }
                }
            }
        }
        Uint64 now = SDL_GetTicksNS();
        float dt = (float)(now - last) / 1e9f;
        last = now;
        render_update_camera(&r, dt, SDL_GetKeyboardState(NULL));
        render_frame(&r);
        if (shot && ++frames == 5) {
            SDL_Surface *s = SDL_RenderReadPixels(r.ren, NULL);
            if (s) { SDL_SaveBMP(s, shot); SDL_DestroySurface(s); }
            running = 0;
        }
        SDL_RenderPresent(r.ren);
    }
    render_shutdown(&r);
    SDL_Quit();
    return 0;
}
