#pragma once

// todo: get rid of these imports
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifndef DETAIL_SRC_W
#define DETAIL_SRC_W 1489
#endif

#ifndef DETAIL_SRC_H
#define DETAIL_SRC_H 1265
#endif

#ifndef DETAIL_REGION_W
#define DETAIL_REGION_W 80
#endif

#ifndef DETAIL_REGION_H
#define DETAIL_REGION_H 80
#endif

#ifndef DETAIL_UPSCALED_W
#define DETAIL_UPSCALED_W 641
#endif

#ifndef DETAIL_UPSCALED_H
#define DETAIL_UPSCALED_H 641
#endif

extern const unsigned char height_data[];
extern const unsigned char height_data_end[];
#define height_data_len ((size_t)(height_data_end - height_data))

extern const unsigned char terrain_data[];
extern const unsigned char terrain_data_end[];
#define terrain_data_len ((size_t)(terrain_data_end - terrain_data))

/*
Required addition in texture.h:

static int upload_detail_texture_pair(
    struct Machine* machine,
    struct Swapchain* swapchain,
    const uint8_t* terrain_pixels,
    const uint8_t* height_pixels,
    uint32_t w,
    uint32_t h
);

This header calls that function.
*/
static int upload_detail_texture_pair(
    struct Machine* machine,
    struct Swapchain* swapchain,
    const uint8_t* terrain_pixels,
    const uint8_t* height_pixels,
    uint32_t w,
    uint32_t h
);

static uint8_t g_detail_terrain[DETAIL_UPSCALED_W * DETAIL_UPSCALED_H];
static uint8_t g_detail_height[DETAIL_UPSCALED_W * DETAIL_UPSCALED_H];

static int detail_clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float detail_clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint8_t detail_sample_bilinear_u8(
    const uint8_t* src,
    int src_w,
    int src_h,
    float x,
    float y
) {
    x = detail_clampf(x, 0.0f, (float)(src_w - 1));
    y = detail_clampf(y, 0.0f, (float)(src_h - 1));

    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    if (x1 >= src_w) x1 = src_w - 1;
    if (y1 >= src_h) y1 = src_h - 1;

    float tx = x - (float)x0;
    float ty = y - (float)y0;

    float s00 = (float)src[y0 * src_w + x0];
    float s10 = (float)src[y0 * src_w + x1];
    float s01 = (float)src[y1 * src_w + x0];
    float s11 = (float)src[y1 * src_w + x1];

    float a = s00 + (s10 - s00) * tx;
    float b = s01 + (s11 - s01) * tx;
    float v = a + (b - a) * ty;

    int iv = (int)(v + 0.5f);
    if (iv < 0) iv = 0;
    if (iv > 255) iv = 255;
    return (uint8_t)iv;
}

/*
Picks an 80x80 source region centered on (center_x, center_y).

For an even-sized window, this uses:
    left = center_x - 40
    top  = center_y - 40

So the covered source texels are:
    x in [left, left + 79]
    y in [top,  top  + 79]

That region is then resampled to 641x641.
*/
static void detail_resample_region_u8(
    const uint8_t* src,
    int src_w,
    int src_h,
    int center_x,
    int center_y,
    int region_w,
    int region_h,
    uint8_t* dst,
    int dst_w,
    int dst_h
) {
    int left = center_x - (region_w / 2);
    int top  = center_y - (region_h / 2);

    int max_left = src_w - region_w;
    int max_top  = src_h - region_h;

    if (max_left < 0) max_left = 0;
    if (max_top < 0) max_top = 0;

    left = detail_clampi(left, 0, max_left);
    top  = detail_clampi(top, 0, max_top);

    for (int y = 0; y < dst_h; ++y) {
        float fy = 0.0f;
        if (dst_h > 1) {
            fy = (float)y * (float)(region_h - 1) / (float)(dst_h - 1);
        }

        for (int x = 0; x < dst_w; ++x) {
            float fx = 0.0f;
            if (dst_w > 1) {
                fx = (float)x * (float)(region_w - 1) / (float)(dst_w - 1);
            }

            float sx = (float)left + fx;
            float sy = (float)top + fy;

            dst[y * dst_w + x] = detail_sample_bilinear_u8(src, src_w, src_h, sx, sy);
        }
    }
}

static int update_detail_region_and_upload(
    struct Machine* machine,
    struct Swapchain* swapchain,
    int center_x,
    int center_y
) {
    if (height_data_len < (size_t)(DETAIL_SRC_W * DETAIL_SRC_H)) {
        printf("height_data is smaller than expected (%zu < %d)\n",
            height_data_len, DETAIL_SRC_W * DETAIL_SRC_H);
        return 0;
    }

    if (terrain_data_len < (size_t)(DETAIL_SRC_W * DETAIL_SRC_H)) {
        printf("terrain_data is smaller than expected (%zu < %d)\n",
            terrain_data_len, DETAIL_SRC_W * DETAIL_SRC_H);
        return 0;
    }

    detail_resample_region_u8(
        terrain_data,
        DETAIL_SRC_W,
        DETAIL_SRC_H,
        center_x,
        center_y,
        DETAIL_REGION_W,
        DETAIL_REGION_H,
        g_detail_terrain,
        DETAIL_UPSCALED_W,
        DETAIL_UPSCALED_H
    );

    detail_resample_region_u8(
        height_data,
        DETAIL_SRC_W,
        DETAIL_SRC_H,
        center_x,
        center_y,
        DETAIL_REGION_W,
        DETAIL_REGION_H,
        g_detail_height,
        DETAIL_UPSCALED_W,
        DETAIL_UPSCALED_H
    );

    return upload_detail_texture_pair(
        machine,
        swapchain,
        g_detail_terrain,
        g_detail_height,
        DETAIL_UPSCALED_W,
        DETAIL_UPSCALED_H
    );
}