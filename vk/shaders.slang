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

// create range per mesh type; implicit mesh id based on instance-id and workgroup
[[vk::binding(0,0)]] StructuredBuffer<uint2>    INSTANCES; // 6 bytes position + 2 bytes cos/sin
// todo: we'll probably need 4 bytes for cos/sin as it includes camera yaw
[[vk::binding(1,0)]] RWStructuredBuffer<uint2>  VISIBLE_PACKED; // 6 bytes position + 2 bytes cos/sin
// we know for a given scene, up front how many instances at most can be visible anyway, except for particles etc.
[[vk::binding(2,0)]] RWStructuredBuffer<mdi>    MESH_INFO; // reuse MDI struct for the mesh info, use instance_count to mean 'max visible of this mesh in this scene'
// one struct per mesh type, index is mesh id
[[vk::binding(3,0)]] RWStructuredBuffer<mdi>    COUNTERS; // MDI
[[vk::binding(4,0)]] StructuredBuffer<uint>     POSITIONS; // R10G10B10A2
[[vk::binding(5,0)]] StructuredBuffer<uint>     NORMALS;   // R10G10B10A2_SNORM
[[vk::binding(6,0)]] StructuredBuffer<uint>     UVS;   // R16G16
[[vk::binding(7,0)]]
cbuffer uniforms {
    uint4 CAMERA_DATA; // packed camera data (position + orientation)
};

[shader("compute")] [numthreads(64,1,1)]
void cs_build_visible(uint3 tid : SV_DispatchThreadID) {
    uint id = tid.x;
    uint instance_count, stride; INSTANCES.GetDimensions(instance_count, stride);
    if (id >= instance_count) return;

    uint2 instance = INSTANCES[id];    // 8 bytes
    uint4 camera   = CAMERA_DATA.xyzw; // 16 bytes

    // get position, represented as dm
    float3 position = float3(
        ((int16_t) (instance.x        & 0xFFFF)) - ((int16_t) (camera.x        & 0xFFFF)),
        ((int16_t)((instance.x >> 16) & 0xFFFF)) - ((int16_t)((camera.x >> 16) & 0xFFFF)),
        ((int16_t) (instance.y        & 0xFFFF)) - ((int16_t) (camera.y        & 0xFFFF))
    );

    // unpack yaw and pitch
    float instance_yaw_cos = float((int8_t)(instance.y >> 16 & 0xFF))   * divide_by_127;
    float instance_yaw_sin = float((int8_t)(instance.y >> 24 & 0xFF))   * divide_by_127;
    float camera_pitch_sin = float((int16_t)(camera.z        & 0xFFFF)) * divide_by_32767;
    float camera_pitch_cos = float((int16_t)(camera.z  >> 16 & 0xFFFF)) * divide_by_32767;
    float camera_yaw_sin   = float((int16_t)(camera.w        & 0xFFFF)) * divide_by_32767;
    float camera_yaw_cos   = float((int16_t)(camera.w  >> 16 & 0xFFFF)) * divide_by_32767;

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

    // convert to int again
    int tx_dm = (int)round(t_view.x);
    int ty_dm = (int)round(t_view.y);
    int tz_dm = (int)round(t_view.z);

    // pack rotated by yaw
    uint packed0 = (uint)((tx_dm & 0xFFFF) | ((ty_dm & 0xFFFF) << 16));
    uint packed1 = (uint)((tz_dm & 0xFFFF) | (((int)round(cos_delta * 127.0f) & 0xFF) << 16) | (((int)round(sin_delta * 127.0f) & 0xFF) << 24));

    if (id == 0) {
        uint slot; InterlockedAdd(COUNTERS[0].instance_count, 1, slot);
        VISIBLE_PACKED[slot] = uint2(packed0, packed1);
    } else if (id == 1) {
        mdi mesh_info = MESH_INFO[id];
        uint slot; InterlockedAdd(COUNTERS[id].instance_count, 1, slot);
        VISIBLE_PACKED[slot + mesh_info.first_instance] = uint2(packed0, packed1);
    }
}

[shader("compute")] [numthreads(1,1,1)]
void cs_prepare_indirect() {
    uint mesh_count, stride; MESH_INFO.GetDimensions(mesh_count, stride);
    for (int mesh_id = 0; mesh_id<mesh_count; ++mesh_id) {
        mdi mesh_info = MESH_INFO[mesh_id];
        COUNTERS[mesh_id].index_count    = mesh_info.index_count;
        COUNTERS[mesh_id].first_index    = mesh_info.first_index;
        COUNTERS[mesh_id].base_vertex    = mesh_info.base_vertex;
        COUNTERS[mesh_id].first_instance = mesh_info.first_instance;
    }
}

struct push_constants {
    uint mode; // 0 = normal mesh draw, 1 = sky
};
[[vk::push_constant]] push_constants PUSH_CONSTANTS;

struct vs_out {
    float4 pos    : SV_Position;                                           // clip-space position
    float2 uv     : TEXCOORD0;                                             // texture coordinates
    float3 normal : TEXCOORD1;                                             // surface normal
};

int3 plane_position(uint VERTEX_ID, uint grid_width) {
    uint row = VERTEX_ID % grid_width;
    uint col = VERTEX_ID / grid_width;
    int cx = (int)(grid_width - 1) / 2;
    int cz = (int)(grid_width - 1) / 2;
    int vx_cm = (int(col) - cx) * 100;
    int vy_cm = 0;
    int vz_cm = (int(row) - cz) * 100;
    return int3(vx_cm, vy_cm, vz_cm);
}

[shader("vertex")]
vs_out vs_main(uint VERTEX_ID : SV_VertexID, [[vk::location(0)]] uint2 INSTANCE, uint DRAW_ID : SV_DrawIndex) {
    // SKY path: fullscreen triangle
    if (PUSH_CONSTANTS.mode == 1) {
        vs_out o;
        float2 p;
        if (VERTEX_ID == 0) p = float2(-1.0,  3.0); // was (-1,-1)
        if (VERTEX_ID == 1) p = float2( 3.0, -1.0);
        if (VERTEX_ID == 2) p = float2(-1.0, -1.0); // was (-1, 3)
        o.pos = float4(p, 1.0, 1.0); // 1.0 / 1.0 -> drawn behind everything else
        o.normal = float3(0.3, 0.5, 1.0);
        o.uv = 0;
        return o;
    }

    // fetch and unpack visible instance data, convert dm to cm (8 bytes)
    uint2 packed_instance = INSTANCE.xy;
    int instance_x = 10 * (int)((int16_t)( packed_instance.x        & 0xFFFF));
    int instance_y = 10 * (int)((int16_t)((packed_instance.x >> 16) & 0xFFFF));
    int instance_z = 10 * (int)((int16_t)( packed_instance.y        & 0xFFFF));
    float cos_d = float(((int8_t)((packed_instance.y >> 16) & 0xFFu))) * divide_by_127;
    float sin_d = float(((int8_t)((packed_instance.y >> 24) & 0xFFu))) * divide_by_127;

    // fetch and unpack vertex position, represented as cm (4 bytes)
    // draw id is essentially just the mesh id in this case
    int3 vertex;
    if (DRAW_ID > 0) {
        vertex = plane_position(VERTEX_ID, 128);
    } else {
        uint packed_vertex = POSITIONS[VERTEX_ID];
        vertex = int3(
            ((int)( packed_vertex        & 0x3FF) << 22) >> 22,
            ((int)((packed_vertex >> 10) & 0x3FF) << 22) >> 22,
            ((int)((packed_vertex >> 20) & 0x3FF) << 22) >> 22
        );
    }

    // fetch and unpack vertex normal (4 bytes)
    uint packed_normal = NORMALS[VERTEX_ID];
    float normal_x = float(((int)( packed_normal        & 0x3FF) << 22) >> 22) * divide_by_511;
    float normal_y = float(((int)((packed_normal >> 10) & 0x3FF) << 22) >> 22) * divide_by_511;
    float normal_z = float(((int)((packed_normal >> 20) & 0x3FF) << 22) >> 22) * divide_by_511;

    // fetch and unpack camera pitch (4 bytes)
    uint packed_camera = CAMERA_DATA.z;
    float camera_pitch_cos = float((int)((int16_t)((packed_camera >> 16) & 0xFFFF))) * divide_by_32767;
    float camera_pitch_sin = float((int)((int16_t)( packed_camera        & 0xFFFF))) * divide_by_32767;

    // rotate position around y for the combined camera-instance yaw we saved
    float x1 =  cos_d * vertex.x + sin_d * vertex.z;
    float z1 = -sin_d * vertex.x + cos_d * vertex.z;
    float y1 =  vertex.y;

    // rotate position around x for the camera pitch
    float y2 =  camera_pitch_cos * y1 + camera_pitch_sin * z1;
    float z2 = -camera_pitch_sin * y1 + camera_pitch_cos * z1;
    float x2 =  x1;

    // move position into the calculated instance-in-camera-space we saved
    float vx = x2 + instance_x;
    float vy = y2 + instance_y;
    float vz = z2 + instance_z;

    // perspective projection (+Z is forward)
    float cx = proj_scale_x * vx;
    float cy = proj_scale_y * vy;
    float cz = depth_a * vz + depth_b;
    float  w = vz;

    vs_out o;
    o.pos = float4(cx, cy, cz, w);
    o.normal = float3(normal_x, normal_y, normal_z);
    o.uv = UVS[VERTEX_ID];
    return o;
}

[shader("fragment")]
float4 fs_main(vs_out i, uint primID : SV_PrimitiveID) : SV_Target0 {
    if (PUSH_CONSTANTS.mode == 1) return float4(i.normal, 1.0);
    // debug draw triangles as distinct colors
    float3 color = frac(sin((primID+43) * 43.5453) * float3(23.1407, 85.928, 43.533));
    if(color.x+color.y+color.z < 0.1) color = float3(float(primID % 32) / 32, float((primID + 38) % 64) / 64, float((primID + 17) % 32) / 32);
    return float4(color, 1.0);
    // return float4(i.normal, 1.0f);
}
