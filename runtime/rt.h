// Native runtime: guest memory, threads, Win32 shims.
#pragma once
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

// ---- guest address space layout ----
#define GUEST_IMAGE_BASE 0x00400000u
#define GUEST_HEAP_BASE  0x01000000u
#define GUEST_HEAP_END   0x70000000u
#define GUEST_VIRT_BASE  0x70000000u   // VirtualAlloc (64 KB aligned)
#define GUEST_VIRT_END   0xC0000000u
#define GUEST_THUNK_BASE 0xFE000000u   // fake addresses that dispatch to native functions
#define GUEST_THUNK_MAX  8192
#define MAGIC_RET        0xFDFFFFF0u   // return address for native -> guest calls

void mem_init(void);
uint32_t gmalloc(uint32_t size);           // zeroed
void gfree(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t size);
uint32_t gsize(uint32_t p);
uint32_t vmalloc(uint32_t addr, uint32_t size);  // VirtualAlloc
uint32_t gstrdup(const char *s);
static inline char *gstr(uint32_t a) { return (char *)PTR(a); }

// ---- shims ----
typedef void (*native_fn)(CPU *R);
uint32_t thunk_new(native_fn fn, const char *name);   // returns a guest address that calls fn
const char *thunk_name(uint32_t addr);
void shim_register(const char *name, native_fn fn);
native_fn shim_lookup(const char *name);
uint32_t shim_addr(const char *name);                 // thunk for a shim (for GetProcAddress)

#define ARG(i) rd32(R->esp + 4 + 4 * (i))
// SHIM(name, nargs): stdcall; nargs = -1 for cdecl
#define SHIM(name, nargs) \
    static uint32_t impl_##name(CPU *R); \
    static void wrap_##name(CPU *R) { uint32_t r = impl_##name(R); R->eax = r; R->esp += 4 + ((nargs) > 0 ? 4 * (nargs) : 0); } \
    __attribute__((constructor)) static void reg_##name(void) { shim_register(#name, wrap_##name); } \
    static uint32_t impl_##name(CPU *R)
// variant whose exported name differs from the C identifier (e.g. decorated C++ names)
#define SHIM_AS(id, expname, nargs) \
    static uint32_t impl_##id(CPU *R); \
    static void wrap_##id(CPU *R) { uint32_t r = impl_##id(R); R->eax = r; R->esp += 4 + ((nargs) > 0 ? 4 * (nargs) : 0); } \
    __attribute__((constructor)) static void reg_##id(void) { shim_register(expname, wrap_##id); } \
    static uint32_t impl_##id(CPU *R)

// ---- native -> guest call ----
void preset_apply_once(CPU *R);   // presets.c
uint32_t guest_call(CPU *R, uint32_t fn, int nargs, ...);   // stdcall or cdecl (esp restored)

// ---- threads / GIL ----
CPU *cpu_current(void);
CPU *cpu_new_thread(void);
void gil_lock(void);
void gil_unlock(void);
void gil_yield(void);

// ---- logs ----
extern int rt_verbose;
#define LOG(...) do { if (rt_verbose) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define WARN(...) do { fprintf(stderr, "[warn] " __VA_ARGS__); fputc('\n', stderr); } while (0)
__attribute__((noreturn)) void rt_fatal(const char *fmt, ...);

// ---- paths ----
extern char g_root[1024];      // game folder (original data)
extern char g_save[1024];      // writable folder (saves/config)
const char *path_map(const char *winpath, char *out, size_t outsz);  // C:\x\y -> native path
uint32_t now_ms(void);

// ---- COM ----
uint32_t com_new(const char *iface, uint32_t vtbl, uint32_t extra_bytes, void *native);
void *com_native(uint32_t obj);
// stdcall COM method: nargs counts the arguments after `this`
#define COMFN(name, nargs) \
    static uint32_t name##_i(CPU *R); \
    static void name(CPU *R) { R->eax = name##_i(R); R->esp += 4 + 4 * ((nargs) + 1); } \
    static uint32_t name##_i(CPU *R)
#define THIS ARG(0)
void com_addref(CPU *R);    // shared IUnknown::AddRef / Release for plain reference-counted objects
void com_release(CPU *R);
uint32_t com_vtbl(const char *iface, const char **methods, int n, native_fn *impls);

// ---- window / platform ----
void plat_init(void);
void plat_pump(void);          // processes SDL events -> Win32 message queue
void plat_present(const uint8_t *pix, int w, int h, int pitch, const uint32_t *pal);
extern uint32_t g_hwnd, g_wndproc;
