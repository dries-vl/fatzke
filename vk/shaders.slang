static const float PI                 = 3.14159265359f;                       // π constant for degree→radian conversions
static const float fov_y_radians      = 60.0f * PI / 180.0f;                  // vertical field of view in radians (60°)
static const float proj_scale_y       = 1.0f / tan(fov_y_radians * 0.5f);     // cot(FoV/2), projection scale factor for Y
static const float aspect_ratio       = 16.0f / 9.0f;                         // screen aspect ratio (width / height)
static const float proj_scale_x       = proj_scale_y / aspect_ratio;          // projection scale factor for X
static const float near_plane         = 0.1f;                                 // near clipping plane distance
static const float far_plane          = 10000.0f;                             // far clipping plane distance
static const float depth_a            = far_plane / (far_plane - near_plane); // depth matrix coefficient A
static const float depth_b            = -far_plane * near_plane / (far_plane - near_plane); // depth matrix coefficient B

struct draw_counters {
    uint index_count;             // number of indices per draw call
    uint instance_count;          // number of visible instances found by compute shader
    uint first_index;             // first index in the index buffer
    int  base_vertex;             // base vertex offset for indexed draw
    uint first_instance;          // first instance index
};

static const uint vertices_per_mesh = 192;   // total vertices per mesh
static const uint indices_per_mesh  = 1116;  // total indices per mesh

// Decode R10G10B10A2_SNORM → float3 in [-1,1]
float3 unpack_r10g10b10a2_snorm(uint packed) {
    int r = (int)( packed        & 0x3FF);
    int g = (int)((packed >> 10) & 0x3FF);
    int b = (int)((packed >> 20) & 0x3FF);
    r = (r << 22) >> 22; g = (g << 22) >> 22; b = (b << 22) >> 22;         // sign-extend 10-bit values
    float s = 1.0f / 511.0f;                                              // normalize from [-511,+511] to [-1,+1]
    return float3(r * s, g * s, b * s);                                   // return normalized color vector
}

// Decode signed X/Z (i16 each) from packed uint → float2 in decimeters
float2 decode_xz_dm(uint packed) {
    int x = (int)(packed & 0xFFFF);      x = (x << 16) >> 16;              // extract signed X
    int z = (int)(packed >> 16);         z = (z << 16) >> 16;              // extract signed Z
    return float2((float)x, (float)z);                                    // return decimeter values
}

// Decode yaw byte → radians [0, 2π)
float decode_yaw_u8(uint packed) {
    return (float(packed & 0xFFu) * (2.0f * PI)) / 255.0f;                // scale byte range to 0..2π
}

// Extract mesh ID from bits [15:8]
uint decode_mesh_id(uint packed) {
    return (packed >> 8) & 0xFFu;                                         // return mesh id
}

// Unpack two signed 16-bit integers from uint
int2 unpack_i16x2(uint packed) {
    int16_t lo = (int16_t)(packed & 0xFFFFu);
    int16_t hi = (int16_t)((packed >> 16) & 0xFFFFu);
    return int2(lo, hi);                                                  // return (lo, hi)
}

// Instance layout: { x_dm:u16 | z_dm:u16 , y_dm:u16 | yaw_cos:i8 | yaw_sin:i8 }
[[vk::binding(0,0)]] StructuredBuffer<uint2> instances;

// Visible layout: { tview.x:i16 | tview.z:i16 , tview.y:i16 | cosΔ:i8 | sinΔ:i8 }
[[vk::binding(1,0)]] RWStructuredBuffer<uint2> visible_packed;

// Per-draw counters (used for indirect draw)
[[vk::binding(2,0)]] RWStructuredBuffer<draw_counters> counters;

// Mesh geometry buffers
[[vk::binding(3,0)]] StructuredBuffer<uint> positions; // R10G10B10A2_SNORM
[[vk::binding(4,0)]] StructuredBuffer<uint> normals;   // R10G10B10A2_SNORM

// Camera data (packed)
// u_cam[0] = cam_x_dm:u16 | cam_y_dm:u16
// u_cam[1] = cam_z_dm:u16 | pitch_cos:i8 | pitch_sin:i8
// u_cam[2] = yaw_cos:i8   | yaw_sin:i8   | (pad16)
[[vk::binding(5,0)]]
cbuffer uniforms {
    uint4 u_cam;                                                           // packed camera data (position + orientation)
};

// Rotate vector by -yaw around Y (camera-space rotation)
static void rotate_y_negative(float2 cos_sin, float3 v, out float3 out_v) {
    float c = cos_sin.x, s = cos_sin.y;
    out_v.x =  c * v.x - s * v.z;                                         // apply -yaw to X/Z
    out_v.z =  s * v.x + c * v.z;                                         // apply -yaw to X/Z
    out_v.y =  v.y;                                                       // Y stays unchanged
}

// Rotate vector by -pitch around X (camera-space rotation)
static void rotate_x_negative(float2 cos_sin, float3 v, out float3 out_v) {
    float c = cos_sin.x, s = cos_sin.y;
    out_v.y =  c * v.y + s * v.z;                                         // apply -pitch to Y/Z
    out_v.z = -s * v.y + c * v.z;                                         // apply -pitch to Y/Z
    out_v.x =  v.x;                                                       // X stays unchanged
}
float2 norm2(float2 v) { return v * rsqrt(max(dot(v,v), 1e-8)); }
void decode_camera(out float3 cam_pos_m, out float2 cam_yaw, out float2 cam_pitch) {
    uint packed_xy = u_cam.x;
    uint packed_zp = u_cam.y;
    uint packed_yaw = u_cam.z;

    int x_dm = (int)((int16_t)(packed_xy & 0xFFFFu));
    int y_dm = (int)((int16_t)((packed_xy >> 16) & 0xFFFFu));
    int z_dm = (int)((int16_t)(packed_zp & 0xFFFFu));
    cam_pos_m = 0.1f * float3(x_dm, y_dm, z_dm);                         // convert decimeters→meters

    int pitch_cos_i8 = (int)((int8_t)((packed_zp >> 16) & 0xFFu));
    int pitch_sin_i8 = (int)((int8_t)((packed_zp >> 24) & 0xFFu));
    int yaw_cos_i8   = (int)((int8_t)( packed_yaw       & 0xFFu));
    int yaw_sin_i8   = (int)((int8_t)((packed_yaw >> 8) & 0xFFu));

    cam_pitch = float2(pitch_cos_i8, pitch_sin_i8) / 127.0f;              // dequantize pitch to [-1,1]
    cam_yaw   = float2(yaw_cos_i8, yaw_sin_i8) / 127.0f;                  // dequantize yaw to [-1,1]
cam_pitch = norm2(cam_pitch);
cam_yaw   = norm2(cam_yaw);
}

[shader("compute")] [numthreads(64,1,1)]
void cs_build_visible(uint3 tid : SV_DispatchThreadID) {
    uint id = tid.x;
    uint instance_count, stride; instances.GetDimensions(instance_count, stride);
    if (id >= instance_count) return;                                     // skip threads beyond instance count

    // Unpack instance
    uint2 packed = instances[id];
    uint packed_xz   = packed.x;
    uint packed_y_yaw = packed.y;

    uint x_dm =  packed_xz        & 0xFFFFu;
    uint z_dm = (packed_xz >> 16) & 0xFFFFu;
    uint y_dm =  packed_y_yaw     & 0xFFFFu;
    int  yaw_cos_i8 = (int)((int8_t)((packed_y_yaw >> 16) & 0xFFu));
    int  yaw_sin_i8 = (int)((int8_t)((packed_y_yaw >> 24) & 0xFFu));

    float3 instance_pos_m = 0.1f * float3(x_dm, y_dm, z_dm);              // convert dm→m
    float2 instance_yaw = norm2(float2(yaw_cos_i8, yaw_sin_i8) / 127.0f);

    // Decode camera
    float3 cam_pos_m;
    float2 cam_yaw, cam_pitch;
    decode_camera(cam_pos_m, cam_yaw, cam_pitch);
    cam_yaw      = norm2(cam_yaw);

    float cos_delta =  instance_yaw.x * cam_yaw.x + instance_yaw.y * cam_yaw.y; // cos(a-b)
    float sin_delta =  instance_yaw.y * cam_yaw.x - instance_yaw.x * cam_yaw.y; // sin(a-b)

    // Transform instance origin into view space
    float3 t_world = instance_pos_m - cam_pos_m;                           // translate relative to camera
    float3 t_yaw, t_view;
    rotate_y_negative(cam_yaw, t_world, t_yaw);                            // apply -yaw
    rotate_x_negative(cam_pitch, t_yaw, t_view);                           // apply -pitch

    // Quantize and pack
    int tx_cm = (int)round(t_view.x);
    int ty_cm = (int)round(t_view.y);
    int tz_cm = (int)round(t_view.z);

    uint packed0 = (uint)((tx_cm & 0xFFFF) | ((tz_cm & 0xFFFF) << 16));   // pack x/z
    uint packed1 = (uint)((ty_cm & 0xFFFF)
                 | (((int)round(cos_delta * 127.0f) & 0xFF) << 16)
                 | (((int)round(sin_delta * 127.0f) & 0xFF) << 24));      // pack y + Δyaw

    uint slot; InterlockedAdd(counters[0].instance_count, 1, slot);        // atomic append
    visible_packed[slot] = uint2(packed0, packed1);                        // write visible instance
}

[shader("compute")] [numthreads(1,1,1)]
void cs_prepare_indirect() {
    counters[0].index_count    = indices_per_mesh;                         // indices per mesh
    counters[0].first_index    = 0;                                        // start index
    counters[0].base_vertex    = 0;                                        // base vertex
    counters[0].first_instance = 0;                                        // first instance index
}

float clamp_unit(float v)   { return max(-1.0f, min(1.0f, v)); }           // clamp to [-1,1]
float replace_nan(float v, float repl) { return (v == v) ? v : repl; }     // replace NaN with fallback

struct vs_out {
    float4 pos    : SV_Position;                                           // clip-space position
    float2 uv     : TEXCOORD0;                                             // texture coordinates
    float3 normal : TEXCOORD1;                                             // surface normal
};

[shader("vertex")]
vs_out vs_main(uint vertex_id : SV_VertexID,
                   [[vk::location(0)]] uint2 visible_data,
                   [[vk::location(1)]] float2 in_uv)
{
    // Unpack visible instance data
    int2 xz_cm = unpack_i16x2(visible_data.x);
    int  y_cm  = (int)((int16_t)(visible_data.y & 0xFFFFu));
    int  cos_q = (int)((int8_t)((visible_data.y >> 16) & 0xFFu));
    int  sin_q = (int)((int8_t)((visible_data.y >> 24) & 0xFFu));

    float3 t_view_m = float3((float)xz_cm.x, (float)y_cm, (float)xz_cm.y);
    float  cos_d = (float)cos_q / 127.0f;
    float  sin_d = (float)sin_q / 127.0f;

    // Fetch vertex data
    float3 pos_local = unpack_r10g10b10a2_snorm(positions[vertex_id]);
    float3 normal = unpack_r10g10b10a2_snorm(normals[vertex_id]);

    // Decode camera pitch
    float2 cam_pitch;
    {
        int cos_i8 = (int)((int8_t)((u_cam[1] >> 16) & 0xFFu));
        int sin_i8 = (int)((int8_t)((u_cam[1] >> 24) & 0xFFu));
        cam_pitch = float2((float)cos_i8, (float)sin_i8) / 127.0f;
    }

    // 1) Rotate around Y by Δyaw
    float x1 =  cos_d * pos_local.x + sin_d * pos_local.z;
    float z1 = -sin_d * pos_local.x + cos_d * pos_local.z;
    float y1 =  pos_local.y;

    // 2) Apply -pitch rotation
    float c = cam_pitch.x, s = cam_pitch.y;
    float y2 =  c * y1 + s * z1;
    float z2 = -s * y1 + c * z1;
    float x2 =  x1;

    // 3) Translate to instance origin
    float vx = x2 + t_view_m.x;
    float vy = y2 + t_view_m.y;
    float vz = z2 + t_view_m.z;

    // Sanitize values
    cos_d = clamp_unit(replace_nan(cos_d, 1.0f));
    sin_d = clamp_unit(replace_nan(sin_d, 0.0f));
    vx = replace_nan(vx, 0.0f);
    vy = replace_nan(vy, 0.0f);
    vz = replace_nan(vz, 1.0f);

    // Perspective projection (+Z forward)
    float cx = proj_scale_x * vx;
    float cy = proj_scale_y * vy;
    float cz = depth_a * vz + depth_b;
    float  w = vz;

    vs_out o;
    o.pos = float4(cx, cy, cz, w);
    o.normal = normal;
    o.uv = in_uv;
    return o;
}

[shader("fragment")]
float4 fs_main(vs_out i) : SV_Target0 {
    return float4(1.0f);
}