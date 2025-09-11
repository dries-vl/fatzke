#include "header.h"

void key_input_callback(void* ud, enum KEYBOARD_BUTTON key, enum INPUT_STATE state)
{
    if (key == KEYBOARD_ESCAPE) {_exit(0);}
}
void mouse_input_callback(void* ud, i32 x, i32 y, enum MOUSE_BUTTON button, enum INPUT_STATE state)
{
}

int main(void){
    WINDOW w = pf_create_window(NULL, key_input_callback,mouse_input_callback);

    // todo: can we not just init everything vulkan at once?
    vk_init(pf_display_or_instance(w), pf_surface_or_hwnd(w));
    vk_create_resources();

    while (1) {
        pf_poll_events(w);
        vk_render_frame(pf_window_width(w), pf_window_height(w));
    }
    return 0;
}
