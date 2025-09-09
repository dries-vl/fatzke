#include "header.h"

void key_input_callback(void* ud, enum KEYBOARD_BUTTON key, enum INPUT_STATE state)
{
    if (key == KEYBOARD_ESCAPE) {_exit(0);}
}
void mouse_input_callback(void* ud, int x, unsigned y, enum MOUSE_BUTTON b, enum INPUT_STATE state)
{
}

int main(void){
    WINDOW w = pf_create_window(0, key_input_callback,mouse_input_callback);

    // todo: can we not just init everything vulkan at once?
    vk_init_instance();
    vk_create_surface(pf_display_or_instance(w), pf_surface_or_hwnd(w));
    vk_choose_phys_and_queue();
    vk_make_device();
    vk_graph_initial_build(pf_window_width(w), pf_window_height(w));

    // todo: this seems to block until input event somewhere instead of vsynced
    while (pf_poll_events(w)){
        pf_timestamp("FRAME");
        if (vk_present_frame()) vk_recreate_all(pf_window_width(w), pf_window_height(w));
    }
    vk_shutdown_all();
    return 0;
}

