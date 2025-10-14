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
#define DEBUG_CPU 1

// VULKAN
#define USE_DISCRETE_GPU 0
#define ENABLE_HDR 0

// LIBC
extern void _exit(int);
extern int printf(const char*,...); // todo: don't use printf
extern int snprintf(char*,usize,const char*,...);
extern void *memcpy(void *__restrict,const void*__restrict,usize);
extern int memcmp(const void*,const void*,usize);
extern void *memset(void*,int,usize);
extern int strcmp(const char*,const char*);
extern char *strdup(const char*);
extern char *strstr(const char*,const char*);
extern usize strlen(const char*);

// PLATFORM
typedef void* WINDOW;
enum MOUSE_BUTTON { MOUSE_MOVED, MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE, MOUSE_BUTTON_UNKNOWN };
enum KEYBOARD_BUTTON { KEYBOARD_ESCAPE, KEYBOARD_BUTTON_UNKNOWN };
enum INPUT_STATE { RELEASED, PRESSED };
typedef void (*KEYBOARD_CB)(void*,enum KEYBOARD_BUTTON,enum INPUT_STATE);
typedef void (*MOUSE_CB)(void*,i32,i32,enum MOUSE_BUTTON,enum INPUT_STATE);
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
