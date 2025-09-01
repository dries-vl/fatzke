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

#pragma region PLATFORM
enum MOUSE_BUTTON { MOUSE_MOVED, MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE, MOUSE_BUTTON_UNKNOWN };
enum KEYBOARD_BUTTON { KEYBOARD_ESCAPE, KEYBOARD_BUTTON_UNKNOWN };
enum INPUT_STATE { RELEASED, PRESSED };
typedef void (*keyboard_cb)(void* ud, enum KEYBOARD_BUTTON key, enum INPUT_STATE state);
typedef void (*mouse_cb)(void* ud, int x, unsigned y, enum MOUSE_BUTTON, enum INPUT_STATE state);
struct WINDOW {u32 width, height; void *display_or_inst, *surface_or_hwnd;};
void pf_time_reset();
void pf_timestamp(char *);
int pf_poll_events(struct WINDOW* w);
struct WINDOW pf_create_window(void *ud, keyboard_cb key_cb, mouse_cb mouse_cb);
#pragma endregion

#pragma region VULKAN
void vk_init_instance(void);
void vk_create_surface(void* display_or_inst, void* surface_or_hwnd);
void vk_make_device(void);
void vk_choose_phys_and_queue(void);
void vk_graph_initial_build(u32 win_w,u32 win_h);
void vk_recreate_all(u32 w,u32 h);
int vk_present_frame(void);
void vk_shutdown_all(void);
#pragma endregion
