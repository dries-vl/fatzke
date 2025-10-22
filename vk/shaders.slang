static const float PI                 = 3.14159265359f;                       // π constant for degree→radian conversions
static const float fov_y_radians      = 60.0f * PI / 180.0f;                  // vertical field of view in radians (60°)
static const float proj_scale_y       = 1.0f / tan(fov_y_radians * 0.5f);     // cot(FoV/2), projection scale factor for Y
static const float aspect_ratio       = 16.0f / 9.0f;                         // screen aspect ratio (width / height)
static const float proj_scale_x       = proj_scale_y / aspect_ratio;          // projection scale factor for X
static const float near_plane         = 5.0f;                                 // near clipping plane distance
static const float divide_by_127      = float(1.0/127.0);
static const float divide_by_511      = float(1.0/511.0);
static const float divide_by_32767    = float(1.0/32767.0);

struct push_constants {
    uint mode; // 0 = normal mesh draw, 1 = sky
};
[[vk::push_constant]] push_constants PUSH_CONSTANTS;

// this will be the 'high level' instance, that can be a group of instances for smaller things
// unit instance: torso + head + shield + weapon
// grass: bunch of grass together (although grass is procedurally generated with noise so not in instance buffer)
// terrain: bunch of tiles together (current 2601 instances split into more granular)
[[vk::binding(0,0)]] StructuredBuffer<uint2>    INSTANCES; // 6 bytes position + 2 bytes cos/sin
// todo: we'll probably need 4 bytes for cos/sin as it includes camera yaw

[[vk::binding(1,0)]] RWStructuredBuffer<uint>     COUNTS;
[[vk::binding(2,0)]] RWStructuredBuffer<uint>     OFFSETS;
[[vk::binding(3,0)]] RWStructuredBuffer<uint>     VISIBLE_IDS;

struct visible {uint a, b, c, pad;};
[[vk::binding(4,0)]] RWStructuredBuffer<visible> VISIBLE; // 6 bytes position + 2 bytes cos/sin

// we know for a given scene, up front how many instances at most can be visible anyway, except for particles etc.
struct mdi {uint index_count, instance_count, first_index, base_vertex, first_instance;};
[[vk::binding(5,0)]] RWStructuredBuffer<mdi>    MESH_INFO; // MDI, instance_count is 'max visible of this mesh in this scene'
[[vk::binding(6,0)]] RWStructuredBuffer<mdi>    DRAW_CALLS;  // MDI, one struct per mesh type, index is mesh id

[[vk::binding(7,0)]] StructuredBuffer<uint>     POSITIONS; // R10G10B10A2
[[vk::binding(8,0)]] StructuredBuffer<uint>     NORMALS;   // R10G10B10A2_SNORM
[[vk::binding(9,0)]] StructuredBuffer<uint>     UVS;       // R16G16_UNORM
[[vk::binding(10,0)]]
cbuffer uniforms {
    uint4 CAMERA_DATA; // packed camera data (position + orientation)
};

uint find_mesh_id(uint instance_id, uint mesh_count) {
    uint lo = 0, hi = mesh_count;
    // log(N) search, finds among 1000 in appr. 10 iterations
    while (lo < hi) {
        uint mid = (lo + hi) >> 1;
        uint start = MESH_INFO[mid].first_instance;
        if (start <= instance_id) lo = mid + 1;
        else hi = mid;
    }
    return (lo - 1) - ((lo-1) % 7);
}

uint choose_lod_step(float z_forward) {
    if (z_forward >= 10000.0) return 0;
    if (z_forward >= 5000.0) return 0;
    if (z_forward >= 2500.0) return 0;
    if (z_forward >= 1250.0) return 0;
    if (z_forward >= 625.0) return 0;
    if (z_forward >= 312.5) return 0;
    if (z_forward >= 1.0) return 0;
    return 0;
}

[shader("compute")] [numthreads(64,1,1)]
void cs_main(uint3 tid : SV_DispatchThreadID) {
    if (PUSH_CONSTANTS.mode == 0) {
        // same fetches for whole workgroup
        uint stride; uint mesh_count; uint instance_count;
        INSTANCES.GetDimensions(instance_count, stride);
        MESH_INFO.GetDimensions(mesh_count, stride);
        uint4 camera = CAMERA_DATA.xyzw; // 16 bytes

        // fetches per instance
        uint instance_id = tid.x; 
        if (instance_id >= instance_count) return; // early out
        uint2 instance = INSTANCES[instance_id];    // 8 bytes
        
        // same calculation for whole workgroup
        int3 camera_position = int3(
            (int16_t) (camera.x        & 0xFFFF),
            (int16_t)((camera.x >> 16) & 0xFFFF),
            (int16_t) (camera.y        & 0xFFFF)
        );
        float camera_pitch_sin = float((int16_t)(camera.z        & 0xFFFF)) * divide_by_32767;
        float camera_pitch_cos = float((int16_t)(camera.z  >> 16 & 0xFFFF)) * divide_by_32767;
        float camera_yaw_sin   = float((int16_t)(camera.w        & 0xFFFF)) * divide_by_32767;
        float camera_yaw_cos   = float((int16_t)(camera.w  >> 16 & 0xFFFF)) * divide_by_32767;
        // unpack instance position and move to camera space
        float3 instance_position = float3(
            ((int16_t) (instance.x        & 0xFFFF)),
            ((int16_t)((instance.x >> 16) & 0xFFFF)),
            ((int16_t) (instance.y        & 0xFFFF))
        );
        float3 position = float3(
            instance_position.x - camera_position.x,
            instance_position.y - camera_position.y,
            instance_position.z - camera_position.z
        );
        // unpack yaw and pitch
        float instance_yaw_cos = float((int8_t)(instance.y >> 16 & 0xFF)) * divide_by_127;
        float instance_yaw_sin = float((int8_t)(instance.y >> 24 & 0xFF)) * divide_by_127;
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

        // pick lod level
        uint base_mesh_id = find_mesh_id(instance_id, mesh_count);
        uint lod_level = choose_lod_step(t_view.z);
        uint mesh_id = base_mesh_id + lod_level;

        // increment counter
        if (lod_level < 7 && (int)round(t_view.z) < 32767 && (int)round(t_view.y) < 32767
            && (int)round(t_view.x) < 32767 && (int)round(t_view.y) >= -camera_position.y
            && position.x < 32767 && position.z < 32767) {
            uint slot; InterlockedAdd(COUNTS[mesh_id], 1, slot);
            VISIBLE_IDS[instance_id] = mesh_id;
        } else {
            VISIBLE_IDS[instance_id] = 0xFFFFFFFF;
        }
    }
    // prefix sum
    else if (PUSH_CONSTANTS.mode == 1) {
        // run on a single thread; tiny work
        if (tid.x != 0) return;
        uint mesh_count, stride; 
        MESH_INFO.GetDimensions(mesh_count, stride);
        static const uint LOD_COUNT = 7;

        for (uint base_mesh_id = 0; base_mesh_id < mesh_count; base_mesh_id += LOD_COUNT) {
            const uint base_instance = MESH_INFO[base_mesh_id].first_instance;
            uint total_count = 0;
            [unroll] for (uint j = 0; j < 7; ++j) {
                uint mesh_id = base_mesh_id + j;
                uint first_instance = base_instance + total_count;
                OFFSETS[mesh_id] = first_instance;
                total_count += COUNTS[mesh_id];
                mdi mi = MESH_INFO[mesh_id];
                DRAW_CALLS[mesh_id].index_count   = mi.index_count;
                DRAW_CALLS[mesh_id].first_index   = mi.first_index;
                DRAW_CALLS[mesh_id].base_vertex   = mi.base_vertex;
                DRAW_CALLS[mesh_id].first_instance = first_instance;
                // zero here, set in next pass to be sure
                DRAW_CALLS[mesh_id].instance_count = 0u;
            }
        }
        return;
    }
    // scatter
    else if (PUSH_CONSTANTS.mode == 2) {
        uint stride, mesh_count, instance_count;
        INSTANCES.GetDimensions(instance_count, stride);
        MESH_INFO.GetDimensions(mesh_count, stride);

        uint instance_id = tid.x;
        if (instance_id >= instance_count) return;

        uint mesh_id = VISIBLE_IDS[instance_id];
        if ((mesh_id) == 0xFFFFFFFF) return;

        // Grab slot via atomic write head on the draw call
        uint slot; InterlockedAdd(DRAW_CALLS[mesh_id].instance_count, 1, slot);
        const uint dst = OFFSETS[mesh_id] + slot;

        // WRITE PACKED INSTANCE
        uint4 camera = CAMERA_DATA.xyzw; // 16 bytes
        uint2 instance = INSTANCES[instance_id];    // 8 bytes
        // same calculation for whole workgroup
        int3 camera_position = int3(
            (int16_t) (camera.x        & 0xFFFF),
            (int16_t)((camera.x >> 16) & 0xFFFF),
            (int16_t) (camera.y        & 0xFFFF)
        );
        float camera_pitch_sin = float((int16_t)(camera.z        & 0xFFFF)) * divide_by_32767;
        float camera_pitch_cos = float((int16_t)(camera.z  >> 16 & 0xFFFF)) * divide_by_32767;
        float camera_yaw_sin   = float((int16_t)(camera.w        & 0xFFFF)) * divide_by_32767;
        float camera_yaw_cos   = float((int16_t)(camera.w  >> 16 & 0xFFFF)) * divide_by_32767;

        // unpack instance position and move to camera space
        float3 instance_position = float3(
            ((int16_t) (instance.x        & 0xFFFF)),
            ((int16_t)((instance.x >> 16) & 0xFFFF)),
            ((int16_t) (instance.y        & 0xFFFF))
        );
        float3 position = float3(
            instance_position.x - camera_position.x,
            instance_position.y - camera_position.y,
            instance_position.z - camera_position.z
        );

        // unpack yaw and pitch
        float instance_yaw_cos = float((int8_t)(instance.y >> 16 & 0xFF)) * divide_by_127;
        float instance_yaw_sin = float((int8_t)(instance.y >> 24 & 0xFF)) * divide_by_127;

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
        uint packed1 = (uint)((tz_dm & 0xFFFF) | ((int)round(cos_delta * 32767.0f) & 0xFFFF) << 16);
        uint packed2 = (uint)(((int)round(sin_delta * 32767.0f) & 0xFFFF));

        VISIBLE[dst] = visible(packed0,packed1,packed2,0);
    }
}

struct vs_out {
    float4 pos    : SV_Position;                                           // clip-space position
    float2 uv     : TEXCOORD0;                                             // texture coordinates
    float3 normal : TEXCOORD1;                                             // surface normal
};

float3 plane_position(uint VERTEX_ID, uint grid_scale) {
    uint grid_width = 128u >> grid_scale;     // 128, 64, 32, ..., 2
    uint row = VERTEX_ID % grid_width;        // 0 .. grid_width-1
    uint col = VERTEX_ID / grid_width;        // 0 .. grid_width-1
    const float span = 128.0 * 100.0;                 // total width per axis
    const float half_span = span * 0.5;       // 63.5
    // Normalize to [0,1] then scale to the fixed span
    float x = (float(col) / float(grid_width - 1)) * span - half_span;
    float z = (float(row) / float(grid_width - 1)) * span - half_span;
    return float3(x, 0.0, z);;
}

[shader("vertex")]
vs_out vs_main(uint VERTEX_ID : SV_VertexID, [[vk::location(0)]] uint4 INSTANCE, uint DRAW_ID : SV_DrawIndex) {
    // SKY path: fullscreen triangle
    if (PUSH_CONSTANTS.mode == 1) {
        vs_out o;
        float2 p;
        if (VERTEX_ID == 0) p = float2(-1.0,  3.0); // was (-1,-1)
        if (VERTEX_ID == 1) p = float2( 3.0, -1.0);
        if (VERTEX_ID == 2) p = float2(-1.0, -1.0); // was (-1, 3)
        o.pos = float4(p, 0.0000001, 1.0); // 1.0 / 1.0 -> drawn behind everything else
        o.normal = float3(0.3, 0.5, 1.0);
        // o.uv = 0;
        return o;
    }
 
    // fetch and unpack visible instance data, convert dm to cm (12 bytes)
    uint3 packed_instance = INSTANCE.xyz;
    int instance_x = 10 * (int)((int16_t)( packed_instance.x        & 0xFFFF));
    int instance_y = 10 * (int)((int16_t)((packed_instance.x >> 16) & 0xFFFF));
    int instance_z = 10 * (int)((int16_t)( packed_instance.y        & 0xFFFF));
    float cos_d = float(((int16_t)((packed_instance.y >> 16) & 0xFFFF))) * divide_by_32767;
    float sin_d = float(((int16_t)((packed_instance.z) & 0xFFFF))) * divide_by_32767;

    // draw id is essentially just the mesh id in this case
    float3 vertex;
    float3 normal;
    float2 uv;
    // todo: better way to recognize the terrain from other meshes
    // todo: 4 bytes yaw instead of 2 bytes -> instance in 16 bytes instead of 8 bytes (?)
    // -> wouldn't 16 bytes x 1M instances be a bit much writes (?)
    // todo: much nicer movement with camera, faster when higher, auto pitch, move forward faster with combined push + arrowkeys
    // todo: even bigger quads for very far away tiles, possibly bigger map also
    // -> use 'task shader' for higher level tiles/groups of instances that could emit only one even bigger scale quad for a bunch of quads
    // -> don't need 4000 instances/quads always, could have much less in the distance, and more granular up close
    if (DRAW_ID < 7) {
        uint grid_scale = 6 - (DRAW_ID % 7);
        vertex = plane_position(VERTEX_ID, grid_scale);
        normal = float3(0.5, 1.0, 0.2);
    } else {
        // we need to know the offset into the position (4 bytes)
        uint base_vertex = MESH_INFO[DRAW_ID].base_vertex;
        // fetch and unpack vertex position, represented as cm (4 bytes)
        uint packed_vertex = POSITIONS[VERTEX_ID + base_vertex];
        vertex = float3(
            float(((int)( packed_vertex        & 0x3FF) << 22) >> 22),
            float(((int)((packed_vertex >> 10) & 0x3FF) << 22) >> 22),
            float(((int)((packed_vertex >> 20) & 0x3FF) << 22) >> 22)
        );
        // fetch and unpack vertex normal (4 bytes)
        uint packed_normal = NORMALS[VERTEX_ID + base_vertex];
        normal = float3(
            float(((int)( packed_normal        & 0x3FF) << 22) >> 22) * divide_by_511,
            float(((int)((packed_normal >> 10) & 0x3FF) << 22) >> 22) * divide_by_511,
            float(((int)((packed_normal >> 20) & 0x3FF) << 22) >> 22) * divide_by_511
        );
        // fetch and unpack uv (4 bytes)
        uint packed_uv = UVS[VERTEX_ID + base_vertex];
        uv = float2(
            (packed_uv & 0xFFFF) * (1.0/65535.0),
            (packed_uv >> 16)    * (1.0/65535.0)
        );
    }

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

    // perspective projection (reverse dept)
    float cx = proj_scale_x * vx;
    float cy = proj_scale_y * vy;
    float cz = near_plane;
    float  w = vz;

    vs_out o = vs_out(float4(-1));
    o.pos = float4(cx, cy, cz, w);
    o.normal = normal;
    o.uv = uv;
    return o;
}

[shader("fragment")]
float4 fs_main(
    vs_out i
    , uint primID : SV_PrimitiveID
) : SV_Target0 {
    if (PUSH_CONSTANTS.mode == 1) return float4(i.normal, 1.0);
    // debug draw triangles as distinct colors
    float3 color = frac(sin((primID+43) * 43.5453) * float3(23.1407, 85.928, 43.533));
    if(color.x+color.y+color.z < 0.1) color = float3(float(primID % 32) / 32, float((primID + 38) % 64) / 64, float((primID + 17) % 32) / 32);
    return float4(color, 1.0);
}
