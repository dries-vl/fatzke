#include "header.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "windows.inc"
#else
#include "wayland.inc"
#endif

#include "vulkan.inc"
#include "battle.inc"

// TODO: vulkan header (keep it clear what the apis are)
void vk_init_instance(void);
void vk_create_surface_wayland(struct wl_display*, struct wl_surface*);
void vk_choose_phys_and_queue(void);
void vk_make_device(void);
void vk_graph_initial_build(u32 w, u32 h);
void vk_recreate_all(u32 w, u32 h);
int  vk_present_frame(void);
void vk_shutdown_all(void);

static void key_input_callback(void* ud, unsigned key, enum INPUT_STATE state)
{
    printf("KEY: %d\n", key);
    // TODO: map the keys from evdev/windows to key enum to handle both here
    if (key == 1) {exit(0);}
}
static void mouse_input_callback(void* ud, int x, unsigned y, enum MOUSE_BUTTON b, enum INPUT_STATE state)
{
    printf("MOUSE: %d\n", b);
}

int main(void){
    struct Window* win = pf_create_window(key_input_callback,mouse_input_callback);

    // todo: can we not just init everything vulkan at once?
    vk_init_instance();
    // todo: can we avoid referring to windows/wayland distinction here though? (apart from which to include)
    #ifdef _WIN32
    vk_create_surface_win32(win->hinst, win->hwnd);
    #else
    vk_create_surface_wayland(win->dpy, win->surf);
    #endif
    vk_choose_phys_and_queue();
    vk_make_device();
    vk_graph_initial_build((u32)win->win_w, (u32)win->win_h);

    // todo: this seems to block until input event somewhere instead of vsynced
    while (pf_poll_events(win)){
        TSTAMP("FRAME");
        if (vk_present_frame()) vk_recreate_all((u32)win->win_w, (u32)win->win_h);
    }
    vk_shutdown_all();
    return 0;
}

void bmain() {
    simulate_battle();
}

