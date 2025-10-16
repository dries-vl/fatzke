#include "header.h"

void key_input_callback(void* ud, enum KEYBOARD_BUTTON key, enum BUTTON_STATE state)
{
    if (key == KEYBOARD_ESCAPE) {_exit(0);}
}
void mouse_input_callback(void* ud, i32 x, i32 y, enum MOUSE_BUTTON button, enum BUTTON_STATE state)
{
}

int main(void){
    WINDOW w = pf_create_window(NULL, key_input_callback,mouse_input_callback);

    vk_init(pf_display_or_instance(w), pf_surface_or_hwnd(w));
    vk_create_resources();

    static u64 frame = 0;

    while (pf_poll_events(w)) {
        // if (!pf_window_visible(w) && frame != 0) {continue;}
        vk_render_frame(pf_window_width(w), pf_window_height(w));
        pf_request_present_feedback(w, ++frame);
    }
    return 0;
}
