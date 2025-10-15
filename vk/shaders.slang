static const float PI                 = 3.14159265359f;                       // π constant for degree→radian conversions
static const float fov_y_radians      = 60.0f * PI / 180.0f;                  // vertical field of view in radians (60°)
static const float proj_scale_y       = 1.0f / tan(fov_y_radians * 0.5f);     // cot(FoV/2), projection scale factor for Y
static const float aspect_ratio       = 16.0f / 9.0f;                         // screen aspect ratio (width / height)
static const float proj_scale_x       = proj_scale_y / aspect_ratio;          // projection scale factor for X
static const float near_plane         = 0.1f;                                 // near clipping plane distance
static const float far_plane          = 10000.0f;                             // far clipping plane distance
static const float depth_a            = far_plane / (far_plane - near_plane); // depth matrix coefficient A
static const float depth_b            = -far_plane * near_plane / (far_plane - near_plane); // depth matrix coefficient B
static const float divide_by_127      = float(1.0/127.0);
static const float divide_by_511      = float(1.0/511.0);
static const float divide_by_32767    = float(1.0/32767.0);

struct mdi {
    uint index_count;             // number of indices per draw call
    uint instance_count;          // number of visible INSTANCES found by compute shader
    uint first_index;             // first index in the index buffer
    int  base_vertex;             // base vertex offset for indexed draw
    uint first_instance;          // first instance index
};

static const uint vertices_per_mesh = 192;   // total vertices per mesh
static const uint indices_per_mesh  = 1116;  // total indices per mesh

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

[[vk::binding(0,0)]] StructuredBuffer<uint2> INSTANCES;
[[vk::binding(1,0)]] RWStructuredBuffer<uint2> VISIBLE_PACKED;
[[vk::binding(2,0)]] RWStructuredBuffer<mdi> COUNTERS;
[[vk::binding(3,0)]] StructuredBuffer<uint> POSITIONS; // R10G10B10A2_SNORM
[[vk::binding(4,0)]] StructuredBuffer<uint> NORMALS;   // R10G10B10A2_SNORM
[[vk::binding(5,0)]]
cbuffer uniforms {
    uint4 CAMERA_DATA; // packed camera data (position + orientation)
};

[shader("compute")] [numthreads(64,1,1)]
void cs_build_visible(uint3 tid : SV_DispatchThreadID) {
    uint id = tid.x;
    uint instance_count, stride; INSTANCES.GetDimensions(instance_count, stride);
    if (id >= instance_count) return;

    uint2 instance = INSTANCES[id];
    uint4 camera   = CAMERA_DATA;

    // get position, represented as dm
    float3 position = float3(
        ((int16_t) (instance.x        & 0xFFFF)) - ((int16_t) (camera.x        & 0xFFFF)),
        ((int16_t)((instance.x >> 16) & 0xFFFF)) - ((int16_t)((camera.x >> 16) & 0xFFFF)),
        ((int16_t) (instance.y        & 0xFFFF)) - ((int16_t) (camera.y        & 0xFFFF))
    );

    // unpack yaw and pitch
    float instance_yaw_cos = float((int8_t)( instance.y >> 16 & 0xFF)) * divide_by_127;
    float instance_yaw_sin = float((int8_t)( instance.y >> 24 & 0xFF)) * divide_by_127;
    float camera_pitch_cos = float((int16_t)(camera.y >> 16   & 0xFFFF)) * divide_by_32767;
    float camera_pitch_sin = float((int16_t)(camera.z         & 0xFFFF)) * divide_by_32767;
    float camera_yaw_cos   = float((int16_t)(camera.z >> 16   & 0xFFFF)) * divide_by_32767;
    float camera_yaw_sin   = float((int16_t)(camera.w         & 0xFFFF)) * divide_by_32767;

    // compute combined yaw between instance and camera
    float cos_delta =  instance_yaw_cos * camera_yaw_cos + instance_yaw_sin * camera_yaw_sin; // cos(a-b)
    float sin_delta =  instance_yaw_sin* camera_yaw_cos - instance_yaw_cos * camera_yaw_sin; // sin(a-b)

    // apply -yaw
    float3 t_yaw;
    t_yaw.x =  camera_yaw_cos * position.x - camera_yaw_sin * position.z; // apply -yaw to X/Z
    t_yaw.z =  camera_yaw_sin * position.x + camera_yaw_cos * position.z; // apply -yaw to X/Z
    t_yaw.y =  position.y;
    // apply -pitch
    float3 t_view;
    t_view.y =  camera_pitch_cos * t_yaw.y + camera_pitch_sin * t_yaw.z; // apply -pitch to Y/Z
    t_view.z = -camera_pitch_sin * t_yaw.y + camera_pitch_cos * t_yaw.z; // apply -pitch to Y/Z
    t_view.x =  t_yaw.x;

    // Quantize and pack
    int tx_cm = (int)round(t_view.x);
    int ty_cm = (int)round(t_view.y);
    int tz_cm = (int)round(t_view.z);

    uint packed0 = (uint)((tx_cm & 0xFFFF) | ((tz_cm & 0xFFFF) << 16));   // pack x/z
    uint packed1 = (uint)((ty_cm & 0xFFFF)
                 | (((int)round(cos_delta * 127.0f) & 0xFF) << 16)
                 | (((int)round(sin_delta * 127.0f) & 0xFF) << 24));      // pack y + Δyaw

    uint slot; InterlockedAdd(COUNTERS[0].instance_count, 1, slot);        // atomic append
    VISIBLE_PACKED[slot] = uint2(packed0, packed1);                        // write visible instance
}

[shader("compute")] [numthreads(1,1,1)]
void cs_prepare_indirect() {
    COUNTERS[0].index_count    = indices_per_mesh;                         // indices per mesh
    COUNTERS[0].first_index    = 0;                                        // start index
    COUNTERS[0].base_vertex    = 0;                                        // base vertex
    COUNTERS[0].first_instance = 0;                                        // first instance index
}

float clamp_unit(float v)   { return max(-1.0f, min(1.0f, v)); }           // clamp to [-1,1]
float replace_nan(float v, float repl) { return (v == v) ? v : repl; }     // replace NaN with fallback

struct vs_out {
    float4 pos    : SV_Position;                                           // clip-space position
    float2 uv     : TEXCOORD0;                                             // texture coordinates
    float3 normal : TEXCOORD1;                                             // surface normal
};

[shader("vertex")]
vs_out vs_main(uint vertex_id : SV_VertexID, [[vk::location(0)]] uint2 visible_data, [[vk::location(1)]] float2 in_uv) {
    // Unpack visible instance data
    int2 xz_cm = unpack_i16x2(visible_data.x) * 10;
    int  y_cm  = (int)((int16_t)(visible_data.y & 0xFFFFu)) * 10;
    int  cos_q = (int)((int8_t)((visible_data.y >> 16) & 0xFFu));
    int  sin_q = (int)((int8_t)((visible_data.y >> 24) & 0xFFu));

    float3 t_view_m = float3((float)xz_cm.x, (float)y_cm, (float)xz_cm.y);
    float  cos_d = (float)cos_q * divide_by_127;
    float  sin_d = (float)sin_q * divide_by_127;

    // fetch vertex data
    uint packed = POSITIONS[vertex_id];
    int x = (int)( packed        & 0x3FF);
    int y = (int)((packed >> 10) & 0x3FF);
    int z = (int)((packed >> 20) & 0x3FF);
    x = (x << 22) >> 22; y = (y << 22) >> 22; z = (z << 22) >> 22;
    float3 vertex = float3(x, y, z);

    packed = NORMALS[vertex_id];
    int r = (int)( packed        & 0x3FF);
    int g = (int)((packed >> 10) & 0x3FF);
    int b = (int)((packed >> 20) & 0x3FF);
    r = (r << 22) >> 22; g = (g << 22) >> 22; b = (b << 22) >> 22;
    float3 normal = float3(r * divide_by_511, g * divide_by_511, b * divide_by_511);

    // Decode camera pitch
    float2 cam_pitch;
    {
        int cos_i8 = (int)((int16_t)((CAMERA_DATA[1] >> 16) & 0xFFFF));
        int sin_i8 = (int)((int16_t)( CAMERA_DATA[2] & 0xFFFF));
        cam_pitch = float2((float)cos_i8, (float)sin_i8) * divide_by_32767;
    }

    // 1) Rotate around Y by Δyaw
    float x1 =  cos_d * vertex.x + sin_d * vertex.z;
    float z1 = -sin_d * vertex.x + cos_d * vertex.z;
    float y1 =  vertex.y;

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