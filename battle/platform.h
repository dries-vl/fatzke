#pragma once
#include "header.h"
#include <time.h>

enum MOUSE_BUTTON
{
    MOUSE_MOVED, MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE, MOUSE_BUTTON_UNKNOWN
};

typedef void (*keyboard_cb)(void* ud, unsigned key, unsigned state);
typedef void (*mouse_cb)(void* ud, int x, unsigned y, unsigned b, unsigned state);

struct PLATFORM_WINDOW
{
    void* platform_data;
    i32 win_w, win_h;
    i32 running;
    i32 vsync_ready;
    i32 mouse_x, mouse_y;
};

struct PLATFORM_WINDOW* pf_create_window(keyboard_cb key_cb, mouse_cb mouse_cb);
int poll_events(struct PLATFORM_WINDOW* w);

inline u64 time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

u64 T0;
#define TINIT() do{T0=time_ns();}while(0)
#define TSTAMP(msg) do{u64 _t=time_ns(); fprintf(stderr,"[+%7.3f ms] %s\n",(_t-T0)/1e6,(msg));}while(0)
