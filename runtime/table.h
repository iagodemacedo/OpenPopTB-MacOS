#pragma once
#include "cpu.h"
typedef struct { uint32_t addr; gfn fn; } FnEntry;
typedef struct { uint32_t iat; const char *dll, *name; } ImportEntry;
// Written by recomp/gen.py into gen*/table.c. The from-source build links them in; the prebuilt app
// compiles them on first launch into a bundle and looks them up with dlsym (runtime/prepare.c).
extern const FnEntry gen_fn_table[];
extern const int gen_fn_table_len;
extern const ImportEntry gen_import_table[];
extern const int gen_import_table_len;
// what the runtime uses, set in main() from one of the two sources above
extern const FnEntry *fn_table;
extern int fn_table_len;
extern const ImportEntry *import_table;
extern int import_table_len;
