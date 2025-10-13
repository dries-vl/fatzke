#include<math.h>
#define PI 3.14159265358979323846f

static inline int   clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int16_t clamp_i16_dm_from_m(float m) {
    int dm = (int)lrintf(m * 10.0f);
    return (int16_t)clamp_int(dm, -32768, 32767);
}
static inline int8_t clamp_i8_from_unit(float v) {
    int q = (int)lrintf(v * 127.0f);
    return (int8_t)clamp_int(q, -128, 127);
}

/* pack exactly what your HLSL expects */
static void encode_uniforms(struct Uniforms* u,
                            float cam_x_m, float cam_y_m, float cam_z_m,
                            i8 yaw, i8 pitch)
{
    int16_t x_dm = clamp_i16_dm_from_m(cam_x_m);
    int16_t y_dm = clamp_i16_dm_from_m(cam_y_m);
    int16_t z_dm = clamp_i16_dm_from_m(cam_z_m);

    float pitch_rad = ((float)pitch / 128.0f) * PI;
    int8_t pc = clamp_i8_from_unit(cosf(pitch_rad));
    int8_t ps = clamp_i8_from_unit(sinf(pitch_rad));
    float yaw_rad = ((float)yaw / 128.0f) * PI;
    int8_t yc = clamp_i8_from_unit(cosf(yaw_rad));
    int8_t ys = clamp_i8_from_unit(sinf(yaw_rad));

    u->uCam[0] = (uint32_t)( (uint16_t)x_dm ) | ((uint32_t)(uint16_t)y_dm << 16);
    u->uCam[1] = (uint32_t)( (uint16_t)z_dm ) | ((uint32_t)(uint8_t)pc   << 16)
                                           | ((uint32_t)(uint8_t)ps   << 24);
    u->uCam[2] = (uint32_t)( (uint8_t)yc )    | ((uint32_t)(uint8_t)ys   << 8);
    u->uCam[3] = 0; /* pad */
}