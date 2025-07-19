// tcc main.c wayland.c -lwayland-client -run
#include <stdio.h>
#include <stdint.h>
#include <time.h>

// sudo apt install libwayland-dev
#include "wayland/wayland.c"

#define HEIGHT 600
#define WIDTH 800 

static void key_input_callback(void *ud, uint32_t key, uint32_t state)
{
    if (key == 1) exit(0);
    if (state) printf("key %u down\n", key);
    else       printf("key %u up\n", key);
}
static void mouse_input_callback(void *ud, int32_t x, int32_t y, uint32_t b)
{
    printf("button %u\n", b);
    printf("pointer at %d,%d\n", x, y);
}

int main(void)
{
    struct ctx *window = create_window(800, HEIGHT, "ultrafast-demo");
    set_input_cb(window, key_input_callback, mouse_input_callback, NULL);

    struct timespec ts = {0};
    int stride;
    uint32_t frame = 0;

    for (;;) {
        /* animate – simple HSV→RGB fake: vary hue over time */
        uint8_t hue = frame;
        uint32_t *px = get_pixels(window, &stride);
        for (int y=0; y<HEIGHT; ++y) {
            uint8_t sat = (uint8_t)(255.0f*y/HEIGHT);
            for (int x=0; x<WIDTH; ++x) {
                uint8_t val = (uint8_t)(255.0f*x/WIDTH);
                /* toy: ARGB = (val, sat, hue, 255) – not real HSV but looks fun */
                px[y*(stride/4)+x] =  (0xFFu<<24) | (hue) | (sat<<8) | (val<<16);
            }
        }
        commit(window);
        frame ++;

        /* event pump & ~60 fps throttle */
        if (!window_poll(window)) break;
        clock_gettime(1, &ts); // 1 is CLOCK_MONOTONIC
        usleep(16666 - ts.tv_nsec/1000 % 16666);
    }
    destroy(window);
}
