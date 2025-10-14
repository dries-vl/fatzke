#pragma once
#include<math.h>
#ifdef _WIN32
#define cosf cos
#define sinf sin
#endif
#define PI 3.14159265358979323846f

static inline int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline i8 clamp_i8_from_unit(float v) {
    int q = (int)lrintf(v * 127.0f);
    return (i8)clamp_int(q, -128, 127);
}

static void encode_uniforms(struct Uniforms* u, i16 x_dm, i16 y_dm, i16 z_dm, i16 yaw, i16 pitch) {
    float pitch_radians = (float)pitch / 32768.0f * PI;
    i8 pitch_cos = clamp_i8_from_unit(cosf(pitch_radians));
    i8 pitch_sin = clamp_i8_from_unit(sinf(pitch_radians));
    float yaw_radians = (float)yaw / 32768.0f * PI;
    i8 yaw_cos = clamp_i8_from_unit(cosf(yaw_radians));
    i8 yaw_sin = clamp_i8_from_unit(sinf(yaw_radians));

    u->uCam[0] = (u32)(u16)x_dm | (u32)(u16)y_dm << 16;
    u->uCam[1] = (u32)(u16)z_dm | (u32)(u8)pitch_cos << 16 | (u32)(u8)pitch_sin << 24;
    u->uCam[2] = (u32)(u8)yaw_cos | (u32)(u8)yaw_sin << 8;
    u->uCam[3] = 0;
}
