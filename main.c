// tcc main.c -lwayland-client -run
#include <stdio.h>
#include <stdint.h>
#include <time.h>

// sudo apt install libwayland-dev
#include "wayland/wayland.c"

#define HEIGHT 1000
#define WIDTH 1000 

static void key_input_callback(void *ud, uint32_t key, uint32_t state)
{
    if (key == 1) exit(0);
    if (state) printf("key %u down\n", key);
    else printf("key %u up\n", key);
}
static void mouse_input_callback(void *ud, int32_t x, int32_t y, uint32_t b)
{
    printf("button %u\n", b);
    printf("pointer at %d,%d\n", x, y);
}

int main(void)
{
    struct ctx *window = create_window(WIDTH, HEIGHT, "<<Fatzke>>");
    set_input_cb(window, key_input_callback, mouse_input_callback, NULL);

    struct timespec ts = {0};
    int stride;
    uint32_t frame = 0;

    for (;;) {
        if (!window_poll(window)) break; // poll for events and break if compositor connection is lost

        uint32_t *buffer = get_pixels(window, &stride);
        // fill the buffer with a solid red color
        for (int y=0; y<HEIGHT; ++y) {
            for (int x=0; x<WIDTH; ++x) {
                // fill with red
                buffer[y*(stride/4)+x] = (0xFFu<<24) | (0xFFu<<16); // ARGB = (red, 0, 0, 255)
            }
        }

        // fill the buffer with a gradient
        uint8_t hue = frame;
        for (int y=0; y<HEIGHT; ++y) {
            uint8_t sat = (uint8_t)(255.0f*y/HEIGHT);
            for (int x=0; x<WIDTH; ++x) {
                uint8_t val = (uint8_t)(255.0f*x/WIDTH);
                /* toy: ARGB = (val, sat, hue, 255) – not real HSV but looks fun */
                buffer[y*(stride/4)+x] =  (0xFFu<<24) | (hue) | (sat<<8) | (val<<16);
            }
        }
        frame += 1000;

        window_wait_vsync(window); // wait for vsync (and keep processing events) before next frame
        commit(window); // tell compositor it can read from the buffer
    }
    destroy(window);
}
