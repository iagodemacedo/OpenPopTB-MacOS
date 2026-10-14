#pragma once
#include <stdint.h>

// queued Win32 messages (filled by SDL events and PostMessage)
// each message belongs to the thread that owns the window (as in Win32); weanetr has its own loop
typedef struct { uint32_t hwnd, msg, wp, lp, time, x, y, tid; } WMsg;
void msgq_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void msgq_post_tid(uint32_t tid, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
int msgq_peek(WMsg *out, uint32_t tid, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove);
extern uint32_t g_main_tid;

// DirectInput state
typedef struct { uint32_t ofs, data, time, seq; } DIEvent;
extern uint8_t di_keys[256];
extern int32_t di_mouse_dx, di_mouse_dy, di_mouse_dz;
extern uint8_t di_mouse_btn[4];
extern int g_mouse_x, g_mouse_y;   // position in game coordinates
void di_push_key(uint32_t dik, int down);
void di_push_mouse(uint32_t ofs, uint32_t data);
int di_pop_key(DIEvent *e);
int di_pop_mouse(DIEvent *e);
int di_key_count(void);
int di_mouse_count(void);
