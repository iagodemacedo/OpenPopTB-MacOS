// Graphics preset of the options screen ("Quick Defaults": Low / Medium / High), applied once at start.
//
// The game keeps only the landscape detail in SAVE/CONFIG00.DAT: the preset itself (screen resolution, sky,
// footsteps, ...) falls back to Low on every launch. The preset is a spin button widget; on a click the game
// advances its value and calls the widget's change callback, which sets the other options accordingly.
// The same is done here on the first frame: set the value, call the callback.
//
// The addresses belong to the builds of the 2018 GOG installer; they are verified at run time
// (the widget must point to the callback), anything else is left alone.
#include "rt.h"
#include <strings.h>

extern const char *g_exe_name;

enum { W_VALUE = 0x30, W_CALLBACK = 0x38 };   // offsets in the widget structure
enum { PRESET_LOW, PRESET_MEDIUM, PRESET_HIGH, PRESET_OFF };

static const struct { const char *exe; uint32_t widget, callback; } hooks[] = {
    {"popTB.exe", 0x57a69c, 0x4d1260},
    {"popTBUW.exe", 0x59e524, 0x48eab0},
    {"D3DPopTB.exe", 0x5d9064, 0x4c54e0},
    {"D3DPopTBUW.exe", 0x597d84, 0x500540},
};

static int wanted_preset(void) {
    const char *e = getenv("POPTB_PRESET");   // low | medium | high | off
    if (!e) return PRESET_HIGH;
    if (!strcasecmp(e, "low")) return PRESET_LOW;
    if (!strcasecmp(e, "medium")) return PRESET_MEDIUM;
    if (!strcasecmp(e, "off")) return PRESET_OFF;
    return PRESET_HIGH;
}

// called from the thread that draws the game's first frame
void preset_apply_once(CPU *R) {
    static int done;
    if (done) return;
    done = 1;
    int preset = wanted_preset();
    if (preset == PRESET_OFF) return;
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++) {
        if (strcmp(g_exe_name, hooks[i].exe)) continue;
        if (rd32(hooks[i].widget + W_CALLBACK) != hooks[i].callback) {
            WARN("graphics preset: unexpected game build, not applied");
            return;
        }
        wr32(hooks[i].widget + W_VALUE, (uint32_t)preset);
        guest_call(R, hooks[i].callback, 1, hooks[i].widget);
        LOG("graphics preset %d applied", preset);
        return;
    }
}
