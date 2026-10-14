#pragma once
#include "cpu.h"
typedef struct { uint32_t addr; gfn fn; } FnEntry;
typedef struct { uint32_t iat; const char *dll, *name; } ImportEntry;
extern const FnEntry fn_table[];
extern const int fn_table_len;
extern const ImportEntry import_table[];
extern const int import_table_len;
