#pragma once
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned u32;
typedef unsigned long long u64;
typedef signed char i8;
typedef short i16;
typedef int i32;
typedef long long i64;
typedef float f32;
typedef double f64;
typedef __SIZE_TYPE__ usize;

// SETTINGS
#define APP_NAME "VK: work in progress"
#define DEBUG_VULKAN 0
#define DEBUG_APP 0
#define DEBUG_CPU 0

// VULKAN
#define USE_DISCRETE_GPU 0
#define ENABLE_HDR 0

// LIBC
extern void _exit(int);
#include <stdio.h>
// extern int printf(const char*,...); // todo: don't use printf
// extern int snprintf(char*,usize,const char*,...);
extern void *memcpy(void *__restrict,const void*__restrict,usize);
extern int memcmp(const void*,const void*,usize);
extern void *memset(void*,int,usize);
extern int strcmp(const char*,const char*);
extern char *strdup(const char*);
extern char *strstr(const char*,const char*);
extern usize strlen(const char*);

// PLATFORM
typedef void* WINDOW;
enum BUTTON_STATE { RELEASED, PRESSED };
enum BUTTON {
    BUTTON_UNKNOWN = 0, 
    MOUSE_MOVED, MOUSE_MARGIN_LEFT, MOUSE_MARGIN_RIGHT, MOUSE_MARGIN_TOP, MOUSE_MARGIN_BOTTOM,
    MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE,
    MOUSE_SCROLL, MOUSE_SCROLL_SIDE,
    KEYBOARD_ESCAPE, KEYBOARD_ENTER, KEYBOARD_BACKSPACE, KEYBOARD_TAB, KEYBOARD_SPACE, KEYBOARD_LEFT, KEYBOARD_RIGHT, KEYBOARD_UP, KEYBOARD_DOWN,
    KEYBOARD_HOME, KEYBOARD_END, KEYBOARD_PAGEUP, KEYBOARD_PAGEDOWN, KEYBOARD_INSERT, KEYBOARD_DELETE,
    KEYBOARD_SHIFT, KEYBOARD_CTRL, KEYBOARD_ALT, KEYBOARD_SUPER,
    KEYBOARD_0, KEYBOARD_1, KEYBOARD_2, KEYBOARD_3, KEYBOARD_4, KEYBOARD_5, KEYBOARD_6, KEYBOARD_7, KEYBOARD_8, KEYBOARD_9,
    KEYBOARD_A, KEYBOARD_B, KEYBOARD_C, KEYBOARD_D, KEYBOARD_E, KEYBOARD_F, KEYBOARD_G, KEYBOARD_H, KEYBOARD_I, KEYBOARD_J, KEYBOARD_K, KEYBOARD_L, KEYBOARD_M, KEYBOARD_N, KEYBOARD_O,
    KEYBOARD_P, KEYBOARD_Q, KEYBOARD_R, KEYBOARD_S, KEYBOARD_T, KEYBOARD_U, KEYBOARD_V, KEYBOARD_W, KEYBOARD_X, KEYBOARD_Y, KEYBOARD_Z,
    KEYBOARD_F1, KEYBOARD_F2, KEYBOARD_F3, KEYBOARD_F4, KEYBOARD_F5, KEYBOARD_F6, KEYBOARD_F7, KEYBOARD_F8, KEYBOARD_F9, KEYBOARD_F10, KEYBOARD_F11, KEYBOARD_F12,
    BUTTON_COUNT
};
typedef void (*KEYBOARD_CB)(void*,enum BUTTON,enum BUTTON_STATE);
typedef void (*MOUSE_CB)(void*,i32,i32,enum BUTTON,int);
u64 pf_ns_now(void);
#ifdef _WIN32 // todo: more elegant solution without ifdefs
u64 pf_ticks_to_ns(u64);
#endif
void pf_time_reset(void);
u64 pf_ns_start(void);
void pf_timestamp(char*);
int pf_window_width(WINDOW);
int pf_window_height(WINDOW);
void *pf_surface_or_hwnd(WINDOW);
void *pf_display_or_instance(WINDOW);
int pf_window_visible(WINDOW);
int pf_poll_events(WINDOW);
WINDOW pf_create_window(void*,KEYBOARD_CB,MOUSE_CB);


static int compare_u32(const void* a, const void* b)
{
    u32 aa = *(const u32*)a;
    u32 bb = *(const u32*)b;
    if (aa < bb) return -1;
    if (aa > bb) return  1;
    return 0;
}