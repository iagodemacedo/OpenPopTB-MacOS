#pragma once
#include <stddef.h>
#include <stdint.h>

// Root of the original game install (folder containing data/, levels/, ...).
void fs_set_root(const char *root);
const char *fs_root(void);

// Loads a whole file relative to the game root. The original ships mixed-case
// names (BL320-0.DAT vs bl320-0.dat) and was written for a case-insensitive FS,
// so lookups fall back to a case-insensitive directory scan.
// Returns malloc'd buffer (caller frees) or NULL.
uint8_t *fs_load(const char *relpath, size_t *out_size);
