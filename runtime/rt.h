// Native runtime: guest memory, threads, Win32 shims.
#pragma once
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

// ---- guest address space layout ----
// POPTB_GUEST_WINDOW (default on WebAssembly): all guest memory lives in the first GUEST_SPACE bytes
// instead of 4 GB, so it fits in wasm32 linear memory next to the runtime. Measured use is a 94 MB
// heap and 8 MB of VirtualAlloc (docs/wasm/PLAN.md §3.4). Thunk addresses and MAGIC_RET stay high:
// they are only compared in dispatch(), never dereferenced.
#if defined(__EMSCRIPTEN__) && !defined(POPTB_GUEST_WINDOW)
#define POPTB_GUEST_WINDOW 1
#endif
#define GUEST_IMAGE_BASE 0x00400000u
#define GUEST_HEAP_BASE  0x01000000u
#ifdef POPTB_GUEST_WINDOW
#define GUEST_SPACE      0x20000000ull // 512 MB
#define GUEST_HEAP_END   0x10000000u   // below WEANETR (0x10000000) and QMixer (0x18000000)
#define GUEST_VIRT_BASE  0x19000000u   // VirtualAlloc (64 KB aligned), above QMixer
#define GUEST_VIRT_END   0x20000000u
#else
#define GUEST_SPACE      (1ull << 32)  // the whole 32-bit space
#define GUEST_HEAP_END   0x70000000u
#define GUEST_VIRT_BASE  0x70000000u   // VirtualAlloc (64 KB aligned)
#define GUEST_VIRT_END   0xC0000000u
#endif
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
uint32_t guest_call(CPU *R, uint32_t fn, int nargs, ...);   // stdcall or cdecl (esp restored)

// ---- threads / GIL ----
CPU *cpu_current(void);
CPU *cpu_new_thread(void);
void gil_lock(void);
void gil_unlock(void);
void gil_yield(void);

// ---- diagnostics (runtime/stats.c, POPTB_STATS=1) ----
extern int stats_on;
extern uint64_t stats_gil_locks;
void stats_init(void);
void stats_thread_start(uint32_t guest_stack_hi);
void stats_thread_label(const char *kind);
void stats_sample(CPU *R);
void stats_report(void);

// ---- logs ----
extern int rt_verbose;
#define LOG(...) do { if (rt_verbose) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define WARN(...) do { fprintf(stderr, "[warn] " __VA_ARGS__); fputc('\n', stderr); } while (0)
__attribute__((noreturn)) void rt_fatal(const char *fmt, ...);
__attribute__((noreturn)) void rt_exit(int code);   // ends the process from any thread

// ---- paths ----
extern char g_root[1024];      // game folder (original data)
extern char g_save[1024];      // writable folder (saves/config)
const char *path_map(const char *winpath, char *out, size_t outsz);  // C:\x\y -> native path
uint32_t now_ms(void);

// ---- COM ----
uint32_t com_new(const char *iface, uint32_t vtbl, uint32_t extra_bytes, void *native);
void *com_native(uint32_t obj);
uint32_t com_vtbl(const char *iface, const char **methods, int n, native_fn *impls);

// ---- window / platform ----
void plat_init(void);
void plat_pump(void);          // processes SDL events -> Win32 message queue
void plat_quit(void);          // shuts SDL down before exiting (WebAssembly only, see rt_exit)
void plat_present(const uint8_t *pix, int w, int h, int pitch, const uint32_t *pal);
void plat_present_hybrid(const uint16_t *pix, int w, int h, int pitch, const uint8_t *mask);
extern uint32_t g_hwnd, g_wndproc;
