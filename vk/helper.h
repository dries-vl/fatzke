#pragma once
#ifdef _WIN32
#define cosf cos
#define sinf sin
#endif
#define PI 3.14159265358979323846f

static void encode_uniforms(struct Uniforms* u, float x_dm, float y_dm, float z_dm, float yaw, float pitch) {
    float pitch_radians = (float)pitch / 32767.0f * PI;
    float pitch_cos = cosf(pitch_radians);
    float pitch_sin = sinf(pitch_radians);
    float yaw_radians = (float)yaw / 32767.0f * PI;
    float yaw_cos = cosf(yaw_radians);
    float yaw_sin = sinf(yaw_radians);

    u->camera_position[0] = x_dm;
    u->camera_position[1] = y_dm;
    u->camera_position[2] = z_dm;
    u->camera_pitch_sin = pitch_sin;
    u->camera_pitch_cos = pitch_cos;
    u->camera_yaw_sin = yaw_sin;
    u->camera_yaw_cos = yaw_cos;
}
