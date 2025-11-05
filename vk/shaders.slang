static const float PI                 = 3.14159265359f;                       // π constant for degree→radian conversions
static const float fov_y_radians      = 60.0f * PI / 180.0f;                  // vertical field of view in radians (60°)
static const float proj_scale_y       = 1.0f / tan(fov_y_radians * 0.5f);     // cot(FoV/2), projection scale factor for Y
static const float aspect_ratio       = 16.0f / 9.0f;                         // screen aspect ratio (width / height)
static const float proj_scale_x       = proj_scale_y / aspect_ratio;          // projection scale factor for X
static const float near_plane         = 5.0f;                                 // near clipping plane distance
static const float divide_by_127      = float(1.0/127.0);
static const float divide_by_511      = float(1.0/511.0);
static const float divide_by_32767    = float(1.0/32767.0);

static const float tanHalfFovY = tan(0.5 * fov_y_radians); // for frustrum culling
static const float tanHalfFovX = tanHalfFovY * aspect_ratio;

struct mdi {uint index_count, instance_count, first_index, base_vertex, first_instance;};
struct di {uint group_count_x, group_count_y, group_count_z;};
struct instance {uint a, b, c, pad;};

// pre-filled data
[[vk::binding(9,0)]]  StructuredBuffer<mdi>    MESH_INFO; // MDI, instance_count is 'max visible of this mesh in this scene'
[[vk::binding(0,0)]]  StructuredBuffer<uint2>  CHUNKS;    // size is # of chunks
[[vk::binding(4,0)]]  StructuredBuffer<uint2>  OBJECTS;   // size is total # of objects in scene
[[vk::binding(11,0)]] StructuredBuffer<uint>   POSITIONS; // R10G10B10A2
[[vk::binding(12,0)]] StructuredBuffer<uint>   NORMALS;   // R10G10B10A2_SNORM
[[vk::binding(13,0)]] StructuredBuffer<uint>   UVS;       // R16G16_UNORM
// chunk buffers
[[vk::binding(1,0)]] RWStructuredBuffer<uint>  VISIBLE_CHUNK_IDS;     // size is # of chunks
[[vk::binding(2,0)]] RWStructuredBuffer<di>    INDIRECT_WORKGROUPS;   // append per 64 directly in chunk pass
// object buffers
[[vk::binding(3,0)]] RWStructuredBuffer<uint>  VISIBLE_OBJECT_COUNT; // compute this in prefix pass
[[vk::binding(7,0)]] RWStructuredBuffer<uint>  VISIBLE_OBJECT_IDS;    // size is # of objects in visible chunks
[[vk::binding(5,0)]] RWStructuredBuffer<uint>  COUNT_PER_MESH;        // size is # of mesh types
[[vk::binding(6,0)]] RWStructuredBuffer<uint>  OFFSET_PER_MESH;       // size is # of mesh types
// final resulting rendering data
[[vk::binding(10,0)]] RWStructuredBuffer<mdi>      DRAW_CALLS;         // MDI, one struct per mesh type, index is mesh id
[[vk::binding(8,0)]]  RWStructuredBuffer<instance> RENDERED_INSTANCES; // size is max # of visible mesh instances
// uniforms
[[vk::binding(14,0)]]
cbuffer uniforms {
    uint4 CAMERA_DATA; // packed camera data (position + orientation)
};
// push constants
struct push_constants {
    uint mode;
};
[[vk::push_constant]] push_constants PUSH_CONSTANTS;

uint find_mesh_id(uint object_id, uint mesh_count) {
    uint lo = 0, hi = mesh_count;
    // log(N) search, finds among 1000 in appr. 10 iterations
    while (lo < hi) {
        uint mid = (lo + hi) >> 1;
        uint start = MESH_INFO[mid].first_instance;
        if (start <= object_id) lo = mid + 1;
        else hi = mid;
    }
    return (lo - 1) - ((lo-1) % 7);
}

static const float d[6] = { 25600.0, 6400.0, 1600.0, 400.0, 100.0, 25.0 };
uint choose_lod_step(float distance) {
    if (distance >= d[0]) return 0; // 2x2 verts
    if (distance >= d[1]) return 1; // 4x4 verts
    if (distance >= d[2]) return 2; // 8x8
    if (distance >= d[3]) return 3; // 16x16
    if (distance >= d[4]) return 4; // 32x32 (1024)
    if (distance >= d[5]) return 5;
    return 6;
}

[shader("compute")] [numthreads(64,1,1)]
void cs_main(uint3 gid: SV_GroupID, uint3 gtid: SV_GroupThreadID, uint3 tid: SV_DispatchThreadID) {
    // CAMERA
    // todo: just store full float camera position + sin/cos values; uniform bandwidth is free anyway
    uint4 camera = CAMERA_DATA.xyzw; // 16 bytes
    float3 camera_position = float3(
        (int16_t) (camera.x        & 0xFFFF),
        (int16_t)((camera.x >> 16) & 0xFFFF),
        (int16_t) (camera.y        & 0xFFFF)
    );
    float camera_pitch_sin = float((int16_t)(camera.z        & 0xFFFF)) * divide_by_32767;
    float camera_pitch_cos = float((int16_t)(camera.z  >> 16 & 0xFFFF)) * divide_by_32767;
    float camera_yaw_sin   = float((int16_t)(camera.w        & 0xFFFF)) * divide_by_32767;
    float camera_yaw_cos   = float((int16_t)(camera.w  >> 16 & 0xFFFF)) * divide_by_32767;

    // CHUNK PASS
    if (PUSH_CONSTANTS.mode == 0) {
        uint stride, chunk_count;
        CHUNKS.GetDimensions(chunk_count, stride);
        uint chunk_id = tid.x;
        if (chunk_id >= chunk_count) return;
        // 64 tile chunks (64 for every 1 actual 8192m chunk)
        // each is 1024m wide
        //
        float3 delta_position;
        if (chunk_id < 64) {
            // assuming each tile is 1024m wide, and we use meters instead of dm now
            float3 chunk_position = float3((chunk_id / 8) * 1024, 0, (chunk_id % 8) * 1024);
            delta_position = chunk_position - (camera_position / 10); // in meters
        } else {
            uint2 object = OBJECTS[chunk_id * 64];    // 8 bytes
            float3 chunk_position = float3(((int16_t) (object.x & 0xFFFF)), ((int16_t)((object.x >> 16) & 0xFFFF)), ((int16_t) (object.y & 0xFFFF)));
            delta_position = chunk_position - camera_position;
        }
        // frustum
        float position_yaw_z =  camera_yaw_sin * delta_position.x + camera_yaw_cos * delta_position.z; // apply yaw to Z
        float3 view = float3(
             camera_yaw_cos   * delta_position.x - camera_yaw_sin   * delta_position.z, // apply yaw to X
             camera_pitch_cos * delta_position.y + camera_pitch_sin * position_yaw_z, // apply pitch to Y (using new z)
            -camera_pitch_sin * delta_position.y + camera_pitch_cos * position_yaw_z // apply pitch to Z (using new z)
        );
        bool frustum = abs(view.x) > (view.z * tanHalfFovX) || abs(view.y) > (view.z * tanHalfFovY);
        if (frustum) return;

        // distance
        float distance = dot(delta_position, delta_position) / 500.0f;
        if (chunk_id < 64 && distance > d[0] + 50000) {
            // if really far, just directly write one visible instance, instead of 64 tile objects
            uint stride; uint mesh_count;
            MESH_INFO.GetDimensions(mesh_count, stride);
            uint base_mesh_id = find_mesh_id(chunk_id * 64, mesh_count);
            uint lod_level = choose_lod_step(distance);
            uint mesh_id = base_mesh_id + lod_level;
            uint slot;  InterlockedAdd(COUNT_PER_MESH[mesh_id], 1, slot);
            uint slot1; InterlockedAdd(VISIBLE_OBJECT_COUNT[0], 1, slot1); // keep a count for later
            VISIBLE_OBJECT_IDS[slot1] = 10000 + chunk_id; // add a visible object
        } else {
            // else spawn a workgroup of 64 objects
            uint slot; InterlockedAdd(INDIRECT_WORKGROUPS[0].group_count_x, 1, slot);
            INDIRECT_WORKGROUPS[0].group_count_y = 1;
            INDIRECT_WORKGROUPS[0].group_count_z = 1;
            VISIBLE_CHUNK_IDS[slot] = chunk_id;
        }
    }
    // OBJECT PASS
    else if (PUSH_CONSTANTS.mode == 1) {
        // same fetches for whole workgroup
        uint stride, mesh_count, object_count, chunk_count;
        OBJECTS.GetDimensions(object_count, stride);
        MESH_INFO.GetDimensions(mesh_count, stride);
        CHUNKS.GetDimensions(chunk_count, stride);
        uint chunk_id = VISIBLE_CHUNK_IDS[gid.x]; // id in global chunk buffer

        // fetches per object
        uint id_within_chunk = gtid.x;
        uint object_id = (chunk_id * 64) + id_within_chunk; // id in global objects buffer
        if (object_id >= object_count) return; // early out

        // unpack object position and move to camera space
        float3 position;
        if (chunk_id < 64) {
            // terrain: assuming each object is 128m wide
            float3 chunk_position = float3((chunk_id / 8) * 1024, 0, (chunk_id % 8) * 1024);
            float3 object_position = float3((id_within_chunk / 8) * 128, 0, (id_within_chunk % 8) * 128);
            object_position += chunk_position;
            position = object_position - (camera_position / 10); // in meters
        } else {
            uint2 object = OBJECTS[object_id]; // 8 bytes
            float3 object_position = float3(((int16_t) (object.x & 0xFFFF)), ((int16_t)((object.x >> 16) & 0xFFFF)), ((int16_t) (object.y & 0xFFFF)));
            position = object_position - camera_position;
        }
        float position_yaw_z =  camera_yaw_sin * position.x + camera_yaw_cos * position.z; // apply yaw to Z
        float3 view = float3(
             camera_yaw_cos   * position.x - camera_yaw_sin   * position.z, // apply yaw to X
             camera_pitch_cos * position.y + camera_pitch_sin * position_yaw_z, // apply pitch to Y (using new z)
            -camera_pitch_sin * position.y + camera_pitch_cos * position_yaw_z // apply pitch to Z (using new z)
        );

        // pick lod level
        uint base_mesh_id = find_mesh_id(object_id, mesh_count);
        float distance = dot(position,position) / 500.0f;
        uint lod_level = choose_lod_step(distance);
        uint mesh_id = base_mesh_id + lod_level;
        
        // calculate frustrum
        bool outside_frustrum = abs(view.x) > (view.z * tanHalfFovX) || abs(view.y) > (view.z * tanHalfFovY);

        bool valid = true;
        // frustrum cull
        if (outside_frustrum) valid = false;
        // overflow cull
        if (view.x > 32767 || view.z > 32767 || view.x < -32767 || view.z < -32767) valid = false;
        // near cull
        if (view.z < 0) valid = false;

        if (valid) {
            // todo: we will write multiple mesh instances here; ie. a torso + head + shield + weapon
            uint slot;  InterlockedAdd(COUNT_PER_MESH[mesh_id], 1, slot);
            // todo: we will use per-workgroup count here to have only one atomic add per 64 (this is possible)
            uint slot1; InterlockedAdd(VISIBLE_OBJECT_COUNT[0], 1, slot1);
            VISIBLE_OBJECT_IDS[slot1] = object_id;
        }
    }
    // prefix sum
    else if (PUSH_CONSTANTS.mode == 2) {
        // run on a single thread; tiny work
        if (tid.x != 0) return;
        uint mesh_count, stride; 
        MESH_INFO.GetDimensions(mesh_count, stride);
        static const uint LOD_COUNT = 7;

        // task 1: calculate the offset for the first instance of each mesh type
        for (uint base_mesh_id = 0; base_mesh_id < mesh_count; base_mesh_id += LOD_COUNT) {
            const uint base_instance = MESH_INFO[base_mesh_id].first_instance;
            uint total_count = 0;
            [unroll] for (uint j = 0; j < 7; ++j) {
                uint mesh_id = base_mesh_id + j;
                uint first_instance = base_instance + total_count;
                OFFSET_PER_MESH[mesh_id] = first_instance;
                total_count += COUNT_PER_MESH[mesh_id];
                mdi mi = MESH_INFO[mesh_id];
                DRAW_CALLS[mesh_id].index_count   = mi.index_count;
                DRAW_CALLS[mesh_id].first_index   = mi.first_index;
                DRAW_CALLS[mesh_id].base_vertex   = mi.base_vertex;
                DRAW_CALLS[mesh_id].first_instance = first_instance;
                // zero here, set in next pass to be sure
                DRAW_CALLS[mesh_id].instance_count = 0u;
            }
        }

        // task 2: fill the indirect dispatch buffer with the sum of visible objects
        INDIRECT_WORKGROUPS[0].group_count_x = (VISIBLE_OBJECT_COUNT[0] + 63) / 64;
        INDIRECT_WORKGROUPS[0].group_count_y = 1;
        INDIRECT_WORKGROUPS[0].group_count_z = 1;
        
        return;
    }
    // scatter
    else if (PUSH_CONSTANTS.mode == 3) {
        uint stride, mesh_count;
        MESH_INFO.GetDimensions(mesh_count, stride);

        uint visible_id = tid.x;
        if (visible_id >= VISIBLE_OBJECT_COUNT[0]) return;

        uint object_id = VISIBLE_OBJECT_IDS[visible_id];

        // WRITE PACKED INSTANCE
        // unpack object position and move to camera space
        uint chunk_id = object_id / 64;
        uint id_within_chunk = object_id - (chunk_id * 64);
        float3 position;
        float object_yaw_cos = 1;
        float object_yaw_sin = 0;
        if (object_id > 10000) {
            chunk_id = object_id - 10000;
            object_id = chunk_id * 64;
            float3 object_position = float3((chunk_id / 8) * 1024, 0, (chunk_id % 8) * 1024);
            position = object_position - (camera_position / 10); // in meters
        } else if (object_id < 64 * 64) {
            float3 chunk_position = float3((chunk_id / 8) * 1024, 0, (chunk_id % 8) * 1024);
            float3 object_position = float3((id_within_chunk / 8) * 128, 0, (id_within_chunk % 8) * 128);
            object_position += chunk_position;
            position = object_position - (camera_position / 10); // in meters
        } else {
            uint2 object = OBJECTS[object_id];    // 8 bytes
            float3 object_position = float3(((int16_t) (object.x & 0xFFFF)), ((int16_t)((object.x >> 16) & 0xFFFF)), ((int16_t) (object.y & 0xFFFF)));
            position = object_position - camera_position;
            // unpack yaw and pitch
            object_yaw_cos = float((int8_t)(object.y >> 16 & 0xFF)) * divide_by_127;
            object_yaw_sin = float((int8_t)(object.y >> 24 & 0xFF)) * divide_by_127;
        }

        float position_yaw_z =  camera_yaw_sin * position.x + camera_yaw_cos * position.z; // apply yaw to Z
        float3 view = float3(
             camera_yaw_cos   * position.x - camera_yaw_sin   * position.z, // apply yaw to X
             camera_pitch_cos * position.y + camera_pitch_sin * position_yaw_z, // apply pitch to Y (using new z)
            -camera_pitch_sin * position.y + camera_pitch_cos * position_yaw_z // apply pitch to Z (using new z)
        );

        // pick lod level
        uint base_mesh_id = find_mesh_id(object_id, mesh_count);
        float distance = dot(position,position) / 500.0f;
        uint lod_level = choose_lod_step(distance);
        uint mesh_id = base_mesh_id + lod_level;
        
        // convert to int again
        int tx_dm = (int)round(view.x);
        int ty_dm = (int)round(view.y);
        int tz_dm = (int)round(view.z);

        // compute combined yaw between object and camera
        float cos_delta =  object_yaw_cos * camera_yaw_cos + object_yaw_sin * camera_yaw_sin; // cos(a-b)
        float sin_delta =  object_yaw_sin* camera_yaw_cos - object_yaw_cos * camera_yaw_sin; // sin(a-b)

        // pack rotated by yaw
        uint packed0 = (uint)((tx_dm & 0xFFFF) | ((ty_dm & 0xFFFF) << 16));
        uint packed1 = (uint)((tz_dm & 0xFFFF) | ((int)round(cos_delta * 32767.0f) & 0xFFFF) << 16);
        uint packed2 = (uint)(((int)round(sin_delta * 32767.0f) & 0xFFFF));

        // Grab slot via atomic write head on the draw call
        uint slot; InterlockedAdd(DRAW_CALLS[mesh_id].instance_count, 1, slot);
        const uint dst = OFFSET_PER_MESH[mesh_id] + slot;
        RENDERED_INSTANCES[dst] = instance(packed0,packed1,packed2,0);
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
        instance_x *= 10;
        instance_y *= 10;
        instance_z *= 10;
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
