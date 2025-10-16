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
typedef long long isize;
typedef unsigned long long usize;

extern void _exit(int);
extern int printf(const char*,...);
extern int snprintf(char*,__SIZE_TYPE__,const char*,...);
extern void *memcpy(void *__restrict,const void*__restrict,__SIZE_TYPE__);
extern int memcmp(const void*,const void*,__SIZE_TYPE__);
extern void *memset(void*,int,__SIZE_TYPE__);
extern int strcmp(const char*,const char*);
extern char *strdup(const char*);
extern char *strstr(const char*,const char*);
extern __SIZE_TYPE__ strlen (const char*);
#define NULL ((void *)0)

#pragma region PLATFORM
typedef void* WINDOW;
enum MOUSE_BUTTON { MOUSE_MOVED, MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE, MOUSE_BUTTON_UNKNOWN };
enum KEYBOARD_BUTTON { KEYBOARD_ESCAPE, KEYBOARD_BUTTON_UNKNOWN };
enum BUTTON_STATE { RELEASED, PRESSED };
typedef void (*KEYBOARD_CB)(void*,enum KEYBOARD_BUTTON,enum BUTTON_STATE);
typedef void (*MOUSE_CB)(void*,i32,i32,enum MOUSE_BUTTON,enum BUTTON_STATE);
u64 pf_ns_now(void);
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
void pf_request_present_feedback(WINDOW,u64);
#pragma endregion

#pragma region SETTINGS
#define APP_NAME "Battle: work in progress"
#define DEBUG 1
#pragma endregion
