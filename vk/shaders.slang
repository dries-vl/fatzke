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

// conceptually a small amount of meta-object types; inf unit, cav unit, plane, ... -> animations based on this type
// -> 'animation set' of a mesh, a mesh can only be used for a given animation set
// let's now just assume there's two animations for units, none for planes
// todo: hardcode 0 animations for planes, hardcode 8 animations for everything else for easy offsetting no fetch needed
struct object_metadata {uint meshes_offset, mesh_count; float radius;};
struct chunk {uint object_type;};
struct mdi {uint index_count, instance_count, first_index, base_vertex, first_instance;};
struct di {uint group_count_x, group_count_y, group_count_z;};
struct instance {uint object_id, animation;};

// pre-filled data
// todo: need to refactor the lod-animation data anyway to be able index into it
// todo: but then we might as well refactor to make the data line up for less bandwidth use
// ie. next frames same vertex is next to in memory
// todo: then might as well do texture compression as perfect lossless is not needed
// current: 1010102; 32bit per position
// astc8x8: 2bit per position
[[vk::binding(14,0)]] StructuredBuffer<uint>            OBJECT_MESH_LIST; // list of all the mesh ids that make up objects
[[vk::binding(15,0)]] StructuredBuffer<object_metadata> OBJECT_METADATA;
[[vk::binding(9,0)]]  StructuredBuffer<mdi>             MESH_INFO; // instance_count is 'max visible of this mesh in this scene'
[[vk::binding(0,0)]]  StructuredBuffer<chunk>  CHUNKS;    // size is # of chunks
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
[[vk::binding(16,0)]]
cbuffer uniforms {
    float3 camera_position;
    float camera_pitch_sin, camera_pitch_cos;
    float camera_yaw_sin, camera_yaw_cos;
    float time; // seconds
    uint selected_object_id;
};
[[vk::binding(17, 0)]] Sampler2D TEXTURES[64];

// push constants
struct push_constants {
    uint mode;
};
[[vk::push_constant]] push_constants PUSH_CONSTANTS;

static const float d[4] = { 64.0, 16.0, 4.0, 1.0 };
uint choose_lod_step(float distance, float radius) {
    if (radius <= 0) radius = 10.0;
    distance = distance / radius;
    if (distance >= d[0]) return 4;
    if (distance >= d[1]) return 3;
    if (distance >= d[2]) return 2;
    if (distance >= d[3]) return 1;
    return 0;
}

groupshared uint counts[25];

[shader("compute")] [numthreads(64,1,1)]
void cs_main(uint3 gid: SV_GroupID, uint3 gtid: SV_GroupThreadID, uint3 tid: SV_DispatchThreadID) {
    // CHUNK PASS
    if (PUSH_CONSTANTS.mode == 0) {
        uint stride, chunk_count;
        CHUNKS.GetDimensions(chunk_count, stride);
        uint chunk_id = tid.x;
        if (chunk_id >= chunk_count) return;
        float3 delta_position;
        if (chunk_id < 6400) {
            // assuming each tile is 1024m wide, and we use meters instead of dm now
            float3 chunk_position = float3(int(chunk_id / 80) * 1024 - 40960 + 512, 0, int(chunk_id % 80) * 1024 - 40960 + 512);
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
        uint radius = 1524;
        bool frustum = (abs(view.x) - radius) > (view.z * tanHalfFovX) || (abs(view.y) - radius) > (view.z * tanHalfFovY);
        if (frustum) return;

        // distance
        float distance = dot(delta_position, delta_position) / 500.0f;
        if (chunk_id < 6400 && distance > d[0] + 50000) {
            // if really far, just directly write one visible instance, instead of 64 tile objects
            uint stride; uint mesh_count;
            MESH_INFO.GetDimensions(mesh_count, stride);
            uint object_type = CHUNKS[chunk_id].object_type;
            uint mesh_offset = OBJECT_METADATA[object_type].meshes_offset;
            uint base_mesh_id = OBJECT_MESH_LIST[mesh_offset];
            uint lod_level = choose_lod_step(distance, OBJECT_METADATA[object_type].radius);
            uint mesh_id = base_mesh_id + lod_level;
            uint slot;  InterlockedAdd(COUNT_PER_MESH[mesh_id], 1, slot);
            uint slot1; InterlockedAdd(VISIBLE_OBJECT_COUNT[0], 1, slot1); // keep a count for later
            VISIBLE_OBJECT_IDS[slot1] = 1000000 + chunk_id; // add a visible object
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
        // workgroup setup
        if (gtid.x < 5) // init to zero
            counts[gtid.x] = 0;
        GroupMemoryBarrierWithGroupSync();
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
        uint radius;
        if (chunk_id < 6400) {
            // terrain: assuming each object is 128m wide
            float3 chunk_position = float3(int(chunk_id / 80) * 1024 - 40960 + 512, 0, int(chunk_id % 80) * 1024 - 40960 + 512);
            float3 object_position = float3(int(id_within_chunk / 8) * 128 - 512 + 64, 0, int(id_within_chunk % 8) * 128 - 512 + 64);
            object_position += chunk_position;
            position = object_position - (camera_position / 10); // in meters
            radius = 150;
        } else {
            uint2 object = OBJECTS[object_id]; // 8 bytes
            float3 object_position = float3(((int16_t) (object.x & 0xFFFF)), ((int16_t)((object.x >> 16) & 0xFFFF)), ((int16_t) (object.y & 0xFFFF)));
            position = object_position - camera_position;
            radius = 32;
        }
        float position_yaw_z =  camera_yaw_sin * position.x + camera_yaw_cos * position.z; // apply yaw to Z
        float3 view = float3(
             camera_yaw_cos   * position.x - camera_yaw_sin   * position.z, // apply yaw to X
             camera_pitch_cos * position.y + camera_pitch_sin * position_yaw_z, // apply pitch to Y (using new z)
            -camera_pitch_sin * position.y + camera_pitch_cos * position_yaw_z // apply pitch to Z (using new z)
        );

        // pick lod level
        // instead of the loop, just have a buffer with object-type info, has a count + an array of meshes
        // then a buffer with mesh info to know the base mesh id for that mesh
        // that also has a count + array of animations + array of offsets per animation
        // -> can read base mesh id + read the offset in frames for that animation + pick frame
        uint object_type = CHUNKS[chunk_id].object_type;
        uint mesh_offset = OBJECT_METADATA[object_type].meshes_offset;
        uint base_mesh_id = OBJECT_MESH_LIST[mesh_offset];
        float distance = dot(position,position) / 500.0f;
        uint lod_level = choose_lod_step(distance, OBJECT_METADATA[object_type].radius);
        uint mesh_id = base_mesh_id + lod_level;
        
        // calculate frustrum
        bool outside_frustrum = (abs(view.x) - radius) > (view.z * tanHalfFovX) || (abs(view.y) - radius) > (view.z * tanHalfFovY);

        bool valid = true;
        // frustrum cull
        if (outside_frustrum) valid = false;
        // overflow cull
        // if ((view.x > 32767 || view.z > 32767 || view.x < -32767 || view.z < -32767) && chunk_id >= 6400) valid = false;
        // near cull
        // if (view.z < 0) valid = false;

        VISIBLE_OBJECT_IDS[tid.x] = valid ? 1 : 0;
        if (valid) {
            uint slot; InterlockedAdd(counts[lod_level], 1, slot);
            // todo: we should use per-workgroup count here to have only one atomic add per 64 (this is possible)
            if (chunk_id >= 6400) {
                // add a head
                uint slot2;  InterlockedAdd(counts[5 + lod_level], 1, slot2);
            }
        }
        GroupMemoryBarrierWithGroupSync();
        if (gtid.x == 0) {
            // bodies: 5 LODs
            [unroll]
            for (uint lod = 0; lod < 5; ++lod) {
                uint c = counts[lod];
                if (c != 0) {
                    InterlockedAdd(COUNT_PER_MESH[mesh_id + lod], c);
                }
            }

            if (chunk_id >= 6400) {
                [unroll]
                for (uint lod = 0; lod < 5; ++lod) {
                    uint c = counts[5 + lod];
                    if (c != 0) {
                        InterlockedAdd(COUNT_PER_MESH[mesh_id + 40], c);
                    }
                }
            }
        }

    }
    // prefix sum
    else if (PUSH_CONSTANTS.mode == 2) {
        if (tid.x != 0) return; // run on a single thread; tiny work
        uint mesh_count, stride; 
        MESH_INFO.GetDimensions(mesh_count, stride);

        // calculate the offset for the first instance of each mesh type
        uint total_count = 0;
        for (uint mesh_id = 0; mesh_id < mesh_count; ++mesh_id) {
            OFFSET_PER_MESH[mesh_id] = total_count;
            mdi mi = MESH_INFO[mesh_id];
            DRAW_CALLS[mesh_id].index_count   = mi.index_count;
            DRAW_CALLS[mesh_id].first_index   = mi.first_index;
            DRAW_CALLS[mesh_id].base_vertex   = mi.base_vertex;
            DRAW_CALLS[mesh_id].first_instance = total_count; // last count is the first here
            DRAW_CALLS[mesh_id].instance_count = 0; // set in scatter pass to have an index returned
            total_count += COUNT_PER_MESH[mesh_id];
        }

        // fill the indirect dispatch buffer for the scatter pass with the sum of visible objects
        // INDIRECT_WORKGROUPS[0].group_count_x = (VISIBLE_OBJECT_COUNT[0] + 63) / 64;
        // INDIRECT_WORKGROUPS[0].group_count_y = 1;
        // INDIRECT_WORKGROUPS[0].group_count_z = 1;
        
        return;
    }
    // scatter
    else if (PUSH_CONSTANTS.mode == 3) {
        uint stride, mesh_count;
        MESH_INFO.GetDimensions(mesh_count, stride);

        uint chunk_id = VISIBLE_CHUNK_IDS[gid.x]; // id in global chunk buffer
        uint id_within_chunk = gtid.x;
        uint object_id = (chunk_id * 64) + id_within_chunk; // id in global objects buffer

        // WRITE PACKED INSTANCE
        // unpack object position and move to camera space
        float3 position;
        float object_yaw_cos = 1;
        float object_yaw_sin = 0;
        bool is_chunk_object = object_id > 1000000;
        if (is_chunk_object) {
            chunk_id = object_id - 1000000;
            object_id = chunk_id * 64;
            float3 object_position = float3(int(chunk_id / 80) * 1024 - 40960 + 512, 0, int(chunk_id % 80) * 1024 - 40960 + 512);
            position = object_position - (camera_position / 10); // in meters
        } else if (object_id < 6400 * 64) {
            float3 chunk_position = float3(int(chunk_id / 80) * 1024 - 40960 + 512, 0, int(chunk_id % 80) * 1024 - 40960 + 512);
            float3 object_position = float3(int(id_within_chunk / 8) * 128 - 512 + 64, 0, int(id_within_chunk % 8) * 128 - 512 + 64);
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
        uint object_type = CHUNKS[chunk_id].object_type;
        uint mesh_offset = OBJECT_METADATA[object_type].meshes_offset;
        uint base_mesh_id = OBJECT_MESH_LIST[mesh_offset];
        float distance = dot(position,position) / 500.0f;
        uint lod_level = choose_lod_step(distance, OBJECT_METADATA[object_type].radius);
        uint mesh_id = base_mesh_id + lod_level;
        
        // convert to int again
        int tx_dm = (int)round(view.x);
        int ty_dm = (int)round(view.y);
        int tz_dm = (int)round(view.z);

        // compute combined yaw between object and camera
        float cos_delta =  object_yaw_cos * camera_yaw_cos + object_yaw_sin * camera_yaw_sin; // cos(a-b)
        float sin_delta =  object_yaw_sin* camera_yaw_cos - object_yaw_cos * camera_yaw_sin; // sin(a-b)

        // Grab slot via atomic write head on the draw call
        uint slot; InterlockedAdd(DRAW_CALLS[mesh_id].instance_count, 1, slot);
        const uint dst = OFFSET_PER_MESH[mesh_id] + slot;
        if (is_chunk_object) {
            RENDERED_INSTANCES[dst] = instance(chunk_id + 1000000, 0);
        } else if (object_id < 6400 * 64) {
            RENDERED_INSTANCES[dst] = instance(object_id, 0);
        } else {
            // ANIMATION
            static const float FRAME_DURATION = 0.5f; // 4 frames per second
            static const float ANIMATION_DURATION = 2.0f;  // 4 frames, 0.5s each
            static const float PAIR_DURATION = ANIMATION_DURATION * 2.0f;
            uint animation = 0;
            uint next_animation = 1;
            float phase = frac(object_id * 0.756688471) * ANIMATION_DURATION; // randomized start time for animations, somewhere within the 2s
            float t_clip = time + phase; // progress with time
            float t_in_clip = frac(t_clip / PAIR_DURATION) * PAIR_DURATION; // 0 to 4s
            float frame_f = t_in_clip / FRAME_DURATION; // 0..8 (two clips is eight total frames)
            uint frame0 = (uint)frame_f; // 0..7
            uint clip0 = frame0 < 4 ? animation : next_animation;
            uint frame1 = (frame0 + 1u) % 8u; // 0..7
            uint clip1 = frame1 < 4 ? animation : next_animation;
            if (frame0 >= 4) frame0 = frame0 - 4; frame0 = (frame0 * 5) + (clip0 * 4 * 5);
            if (frame1 >= 4) frame1 = frame1 - 4; frame1 = (frame1 * 5) + (clip1 * 4 * 5);
            float alpha  = frac(frame_f); // interpolation factor between the frame
            uint packed_anim = ((frame0  & 0x3FF)) | ((frame1  & 0x3FF) << 10) | (((uint)round(alpha * 1023.0f) & 0x3FF) << 20);
            // ADD MESHES
            // add the body
            RENDERED_INSTANCES[dst] = instance(object_id, packed_anim);
            // add the head
            uint slot_head; InterlockedAdd(DRAW_CALLS[mesh_id + 40].instance_count, 1, slot_head);
            const uint head_dst = OFFSET_PER_MESH[mesh_id + 40] + slot_head;
            RENDERED_INSTANCES[head_dst] = instance(object_id, packed_anim);
        }
    }
}

struct vs_out {
    float4 pos    : SV_Position;                                           // clip-space position
    float2 uv     : TEXCOORD0;                                             // texture coordinates
    float3 normal : TEXCOORD1;                                             // surface normal
    uint object_id : TEXCOORD2;
};

static const uint MAX_VERTICES = 32u;
static const float TILE_SIZE_CM = 128.0 * 100.0; // in cm
float3 plane_position(uint VERTEX_ID, uint grid_scale) {
    uint vertices_per_side = MAX_VERTICES >> grid_scale;     // 32, 16, 8, 4, 2 (ie. scale 6 becomes 2 -> quad)
    uint row = VERTEX_ID % vertices_per_side;        // 0 .. vertices_per_side-1
    uint col = VERTEX_ID / vertices_per_side;        // 0 .. vertices_per_side-1
    // Normalize to [0,1] then scale to the fixed tile size
    float x = (float(col) / float(vertices_per_side - 1)) * TILE_SIZE_CM - (TILE_SIZE_CM / 2);
    float z = (float(row) / float(vertices_per_side - 1)) * TILE_SIZE_CM - (TILE_SIZE_CM / 2);
    return float3(x, 0.0, z);
}

[shader("vertex")]
vs_out vs_main(uint VERTEX_ID : SV_VertexID, [[vk::location(0)]] instance INSTANCE, uint DRAW_ID : SV_DrawIndex) {
    // SKY path: fullscreen triangle
    if (PUSH_CONSTANTS.mode == 1) {
        vs_out o;
        float2 p;
        if (VERTEX_ID == 1) p = float2(-1.0,  3.0); // was (-1,-1)
        if (VERTEX_ID == 0) p = float2( 3.0, -1.0);
        if (VERTEX_ID == 2) p = float2(-1.0, -1.0); // was (-1, 3)
        o.pos = float4(p, 0.0000001, 1.0); // 1.0 / 1.0 -> drawn behind everything else
        o.normal = float3(0.3, 0.5, 1.0);
        o.object_id = 0;
        return o;
    }
 
    // fetch and unpack visible instance data, convert dm to cm (12 bytes)
    float instance_x;
    float instance_y;
    float instance_z;
    float cos_d = 1.0;
    float sin_d = 0.0;

    // draw id is essentially just the mesh id in this case
    float3 vertex;
    float3 normal;
    float2 uv;
    uint object_id = INSTANCE.object_id;
    if (DRAW_ID < 5) {
        bool is_chunk_object = object_id > 1000000;
        if (is_chunk_object) object_id -= 1000000;
        uint chunk_id = object_id / 64;
        if (is_chunk_object) chunk_id = object_id;
        uint id_within_chunk = object_id - (chunk_id * 64);
        float3 chunk_position = float3(int(chunk_id / 80) * 1024 - 40960 + 512, 0, int(chunk_id % 80) * 1024 - 40960 + 512);
        float3 object_position = float3(int(id_within_chunk / 8) * 128 - 512 + 64, 0, int(id_within_chunk % 8) * 128 - 512 + 64);
        object_position += chunk_position;
        if (is_chunk_object) object_position = chunk_position;
        float3 position = object_position - (camera_position / 10); // in meters
        float position_yaw_z =  camera_yaw_sin * position.x + camera_yaw_cos * position.z; // apply yaw to Z
        float3 view = float3(
             camera_yaw_cos   * position.x - camera_yaw_sin   * position.z, // apply yaw to X
             camera_pitch_cos * position.y + camera_pitch_sin * position_yaw_z, // apply pitch to Y (using new z)
            -camera_pitch_sin * position.y + camera_pitch_cos * position_yaw_z // apply pitch to Z (using new z)
        );
        cos_d = 1.0 * camera_yaw_cos + 0.0 * camera_yaw_sin; // cos(a-b)
        sin_d = 0.0 * camera_yaw_cos - 1.0 * camera_yaw_sin; // sin(a-b)
        instance_x = 100.0 * view.x; // in cm
        instance_y = 100.0 * view.y;
        instance_z = 100.0 * view.z;
        vertex = plane_position(VERTEX_ID, DRAW_ID); // in cm
        if (is_chunk_object) vertex *= 8;
        int2 vertex_world = int2(object_position.xz * 100 + vertex.xz);
        normal = float3(0.5, 1.0, 0.2);
        uv = (object_position.xz + (vertex.xz / 100.0) + (128.0 * 8.0 * 80.0 / 2.0)) / (128.0 * 8.0 * 80.0);
        uv = float2(1,1) + float2(uv.x, -uv.y);
    } else {
        // instance
        uint2 object = OBJECTS[object_id];    // 8 bytes
        float3 object_position = float3(((int16_t) (object.x & 0xFFFF)), ((int16_t)((object.x >> 16) & 0xFFFF)), ((int16_t) (object.y & 0xFFFF)));
        float3 position = object_position - camera_position;
        // unpack yaw and pitch
        float object_yaw_cos = float((int8_t)(object.y >> 16 & 0xFF)) * divide_by_127;
        float object_yaw_sin = float((int8_t)(object.y >> 24 & 0xFF)) * divide_by_127;
        float position_yaw_z =  camera_yaw_sin * position.x + camera_yaw_cos * position.z; // apply yaw to Z
        float3 view = float3(
             camera_yaw_cos   * position.x - camera_yaw_sin   * position.z, // apply yaw to X
             camera_pitch_cos * position.y + camera_pitch_sin * position_yaw_z, // apply pitch to Y (using new z)
            -camera_pitch_sin * position.y + camera_pitch_cos * position_yaw_z // apply pitch to Z (using new z)
        );
        instance_x = 10.0 * view.x; // in cm
        instance_y = 10.0 * view.y;
        instance_z = 10.0 * view.z;
        cos_d =  object_yaw_cos * camera_yaw_cos + object_yaw_sin * camera_yaw_sin; // cos(a-b)
        sin_d =  object_yaw_sin* camera_yaw_cos - object_yaw_cos * camera_yaw_sin; // sin(a-b)
        uint packed_anim = INSTANCE.animation;
        uint frame0  =  packed_anim        & 0x3FF;        // 10 bits
        uint frame1  = (packed_anim >> 10) & 0x3FF;        // 10 bits
        float alpha = (float)((packed_anim >> 20) & 0x3FF) / 1023.0f;   // 10 bits
        uint base_vertex = MESH_INFO[DRAW_ID + frame0].base_vertex;
        uint base_vertex_next = MESH_INFO[DRAW_ID + frame1].base_vertex;
        // fetch and unpack vertex position, represented as cm (4 bytes)
        uint packed_vertex = POSITIONS[VERTEX_ID + base_vertex];
        uint packed_vertex_next = POSITIONS[VERTEX_ID + base_vertex_next];
        vertex = float3(
            float(((int)( packed_vertex        & 0x3FF) << 22) >> 22),
            float(((int)((packed_vertex >> 10) & 0x3FF) << 22) >> 22),
            float(((int)((packed_vertex >> 20) & 0x3FF) << 22) >> 22)
        );
        float3 vertex_next = float3(
            float(((int)( packed_vertex_next        & 0x3FF) << 22) >> 22),
            float(((int)((packed_vertex_next >> 10) & 0x3FF) << 22) >> 22),
            float(((int)((packed_vertex_next >> 20) & 0x3FF) << 22) >> 22)
        );
        vertex = lerp(vertex, vertex_next, alpha);
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
    o.object_id = INSTANCE.object_id;
    return o;
}

struct ps_out {
    float4 color : SV_Target0; // normal color
    uint   id    : SV_Target1; // object ID
};

[shader("fragment")]
ps_out fs_main(vs_out i, uint primID : SV_PrimitiveID) : SV_Target0 {
    ps_out o;
    o.id = i.object_id;
    if (PUSH_CONSTANTS.mode == 1) return ps_out(float4(i.normal, 1.0), i.object_id);
    // // debug draw triangles as distinct colors
    float3 color = frac(sin((primID+43) * 43.5453) * float3(23.1407, 85.928, 43.533));
    if(color.x+color.y+color.z < 0.1) color = float3(float(primID % 32) / 32, float((primID + 38) % 64) / 64, float((primID + 17) % 32) / 32);
    // return float4(color, 1.0);
    // float4 color = TEXTURES[0].Sample(i.uv);
    float4 output = TEXTURES[1].Sample(i.uv);
    output.xyz += color / 5;
    o.color = output;
    if (i.object_id == selected_object_id) {
        o.color.xyz += float3(0.2);
    }
    return o;
}
