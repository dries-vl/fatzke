/* compile: gcc -O3 -pipe -march=native -fno-stack-protector \
             -fno-asynchronous-unwind-tables -s demo.c ultrafast.c \
             -lwayland-client -lrt -o demo */

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include "wayland.h"

static void key(void *ud, uint32_t key, uint32_t state)
{
    if (state) printf("key %u down\n", key);
    else       printf("key %u up\n", key);
}
static void ptr(void *ud, int32_t x, int32_t y, uint32_t b)
{
    printf("button %u\n", b);
}

int main(void)
{
    uf_ctx *u = uf_create_window(800, 600, "ultrafast-demo");
    uf_set_input_cb(u, key, ptr, NULL);

    struct timespec ts={0};
    int stride;
    uint32_t frame = 0;

    for (;;) {
        /* animate – simple HSV→RGB fake: vary hue over time */
        uint8_t hue = frame;
        uint32_t *px = uf_get_pixels(u, &stride);
        int w = 800, h = 600;
        for (int y=0; y<h; ++y) {
            uint8_t sat = (uint8_t)(255.0f*y/h);
            for (int x=0; x<w; ++x) {
                uint8_t val = (uint8_t)(255.0f*x/w);
                /* toy: ARGB = (val, sat, hue, 255) – not real HSV but looks fun */
                px[y*(stride/4)+x] =  (0xFFu<<24) | (hue) | (sat<<8) | (val<<16);
            }
        }
        uf_commit(u);
        ++frame;

        /* event pump & ~60 fps throttle */
        if (!uf_poll(u)) break;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        usleep(16666 - ts.tv_nsec/1000 % 16666);
    }
    uf_destroy(u);
}
