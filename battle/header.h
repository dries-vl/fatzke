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
extern int printf(const char*, ...);

#pragma region PLATFORM
enum MOUSE_BUTTON { MOUSE_MOVED, MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE, MOUSE_BUTTON_UNKNOWN };
enum KEYBOARD_BUTTON { KEYBOARD_ESCAPE, KEYBOARD_BUTTON_UNKNOWN };
enum INPUT_STATE { RELEASED, PRESSED };
typedef void (*keyboard_cb)(void* ud, enum KEYBOARD_BUTTON key, enum INPUT_STATE state);
typedef void (*mouse_cb)(void* ud, i32 x, unsigned y, enum MOUSE_BUTTON, enum INPUT_STATE state);
typedef void* WINDOW;
void pf_time_reset();
void pf_timestamp(char*);
int pf_window_width(WINDOW);
int pf_window_height(WINDOW);
void *pf_surface_or_hwnd(WINDOW);
void *pf_display_or_instance(WINDOW);
int pf_window_visible(WINDOW);
int pf_poll_events(WINDOW);
WINDOW pf_create_window(void *ud, keyboard_cb key_cb, mouse_cb mouse_cb);
#pragma endregion

#pragma region VULKAN
void vk_init_instance(void);
void vk_create_surface(void*, void*);
void vk_make_device(void);
void vk_choose_phys_and_queue(void);
void vk_graph_initial_build(u32,u32);
int  vk_present_frame(void);
void vk_recreate_all(u32,u32);
void vk_shutdown_all(void);
#pragma endregion

