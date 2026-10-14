#pragma once
#include "rt.h"
#include <pthread.h>

typedef struct NThread {
    CPU cpu;
    uint32_t tls[64];
    uint32_t last_error;
    uint32_t tid;
    uint32_t tib;
    uint32_t stack_lo, stack_hi;
    uint32_t handle;
} NThread;

enum { H_NONE, H_FILE, H_EVENT, H_SEM, H_MUTEX, H_THREAD, H_FIND, H_MAPPING, H_HEAP, H_MODULE, H_MISC };

typedef struct Handle {
    int type;
    int fd;                 // H_FILE
    char *path;
    int signaled, manual;   // H_EVENT
    int count, max;         // H_SEM
    uint32_t owner;         // H_MUTEX
    int done; uint32_t exit_code; pthread_t th; int suspended; // H_THREAD
    void *find;             // H_FIND
    uint32_t file;          // H_MAPPING: file handle
} Handle;

uint32_t handle_new(int type);
Handle *handle_get(uint32_t h);
void handle_close(uint32_t h);
void handles_signal(void);    // wakes threads waiting on objects

NThread *nthread(CPU *R);
uint32_t thread_setup_tib(NThread *t);
void set_last_error(CPU *R, uint32_t e);
