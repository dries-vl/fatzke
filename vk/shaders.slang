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

[[vk::binding(14,0)]] StructuredBuffer<uint>            OBJECT_MESH_LIST; // list of all the mesh ids that make up objects
[[vk::binding(15,0)]] StructuredBuffer<object_metadata> OBJECT_METADATA;
[[vk::binding(9,0)]]  StructuredBuffer<mdi>             MESH_INFO; // instance_count is 'max visible of this mesh in this scene'
[[vk::binding(0,0)]]  StructuredBuffer<chunk>  CHUNKS;    // size is # of chunks
[[vk::binding(4,0)]]  StructuredBuffer<uint2>  OBJECTS;   // size is total # of objects in scene
[[vk::binding(11,0)]] StructuredBuffer<uint>   POSITIONS; // R10G10B10A2
[[vk::binding(12,0)]] StructuredBuffer<uint>   NORMALS;   // R10G10B10A2_SNORM
[[vk::binding(13,0)]] StructuredBuffer<uint>   UVS;       // R16G16_UNORM
// chunk buffers
[[vk::binding(1,0)]]  RWStructuredBuffer<uint>  VISIBLE_CHUNK_IDS;     // size is # of chunks
[[vk::binding(2,0)]]  RWStructuredBuffer<di>    INDIRECT_WORKGROUPS;   // append per 64 directly in chunk pass
// object buffers
[[vk::binding(3,0)]]  RWStructuredBuffer<uint>  VISIBLE_OBJECT_COUNT; // compute this in prefix pass
[[vk::binding(7,0)]]  RWStructuredBuffer<uint>  VISIBLE_OBJECT_IDS;   // size is # of objects in visible chunks
[[vk::binding(5,0)]]  RWStructuredBuffer<uint>  COUNT_PER_MESH;       // size is # of mesh types
[[vk::binding(6,0)]]  RWStructuredBuffer<uint>  OFFSET_PER_MESH;      // size is # of mesh types
[[vk::binding(16,0)]] RWStructuredBuffer<uint>  BUCKETS; // 8mb for 131k buckets, need about 4x the amount of used 2mx2m tiles, appr. 2x the amount of units, this should work up to 60k units
// final resulting rendering data
[[vk::binding(10,0)]] RWStructuredBuffer<mdi>      DRAW_CALLS;         // MDI, one struct per mesh type, index is mesh id
[[vk::binding(8,0)]]  RWStructuredBuffer<instance> RENDERED_INSTANCES; // size is max # of visible mesh instances
// uniforms
[[vk::binding(17,0)]]
cbuffer uniforms {
    float3 camera_position;
    float  camera_pitch_sin, camera_pitch_cos;
    float  camera_yaw_sin, camera_yaw_cos;
    float  time; // seconds
    uint   selected_object_id;
    float  drag_rect_start_x, drag_rect_start_y, drag_rect_end_x, drag_rect_end_y;
};
[[vk::binding(18, 0)]] Sampler2D TEXTURES[64];

// push constants
struct push_constants {
    uint mode;
};
[[vk::push_constant]] push_constants PUSH_CONSTANTS;

static const float d[4] = { 512.0, 128.0, 32.0, 8.0 };
uint choose_lod_step(float distance, float radius) {
    if (radius <= 0) radius = 10.0;
    distance = distance / radius;
    if (distance >= d[0]) return 4;
    if (distance >= d[1]) return 3;
    return 2;
    //if (distance >= d[2]) return 2;
    //if (distance >= d[3]) return 1;
    //return 0;
}

static const uint bucket_size = 2; // size in meters of each bucket ~2mx2m
static const uint map_size = 4096; // 4096m
static const uint buckets_width = map_size / bucket_size;
uint get_bucket_id(float x, float y) {
    uint key = uint(floor(y / bucket_size) )* buckets_width + uint(floor(x / bucket_size));
    return (key ^ (key >> 15)) & 0x1FFFFu; // take 17 bits for result between 0 and 131k
}

groupshared uint counts[25];

[shader("compute")] [numthreads(64,1,1)]
void cs_main(uint3 gid: SV_GroupID, uint3 gtid: SV_GroupThreadID, uint3 tid: SV_DispatchThreadID) {
    // CHUNK PASS
    if (gtid.x < 25) // init to zero
        counts[gtid.x] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (PUSH_CONSTANTS.mode == 0) {
        uint stride, chunk_count;
        CHUNKS.GetDimensions(chunk_count, stride);
        uint chunk_id = tid.x;
        if (chunk_id >= chunk_count) return;
        float3 delta_position;
        float3 chunk_position;
        if (chunk_id < 6400) {
            // assuming each tile is 1024m wide, and we use meters instead of dm now
            chunk_position = float3(int(chunk_id / 80) * 1024 - 40960 + 512, 0, int(chunk_id % 80) * 1024 - 40960 + 512);
            delta_position = chunk_position - (camera_position / 10); // in meters
        } else {
            uint2 object = OBJECTS[chunk_id * 64];    // 8 bytes
            chunk_position = float3(((int16_t) (object.x & 0xFFFF)), ((int16_t)((object.x >> 16) & 0xFFFF)), ((int16_t) (object.y & 0xFFFF)));
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
        bool valid = true;
        bool frustum = (abs(view.x) - radius) > (view.z * tanHalfFovX) || (abs(view.y) - radius) > (view.z * tanHalfFovY);
        // if (frustum && view.z > 0) valid = false;
        if (valid) {
            // distance
            float distance = dot(delta_position, delta_position) / 500.0f;
            if (chunk_id < 6400 && (chunk_position.x < -2048 || chunk_position.x >= 2048 || chunk_position.z < -2048 || chunk_position.z >= 2048)) {
            //if (chunk_id < 6400 && distance > d[0] + 50000) {
                // if really far, just directly write one visible instance, instead of 64 tile objects
                uint stride; uint mesh_count;
                MESH_INFO.GetDimensions(mesh_count, stride);
                uint object_type = CHUNKS[chunk_id].object_type;
                // uint lod_level = choose_lod_step(distance, OBJECT_METADATA[object_type].radius * 20);
                uint lod_level = 3;
                uint slot; InterlockedAdd(counts[lod_level], 1, slot);
            } else {
                // else spawn a workgroup of 64 objects
                uint slot; InterlockedAdd(INDIRECT_WORKGROUPS[0].group_count_x, 1, slot);
                INDIRECT_WORKGROUPS[0].group_count_y = 1;
                INDIRECT_WORKGROUPS[0].group_count_z = 1;
                VISIBLE_CHUNK_IDS[slot] = chunk_id;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        if (gtid.x == 0) {
            // bodies: 5 LODs
            uint object_type = CHUNKS[chunk_id].object_type;
            uint mesh_offset = OBJECT_METADATA[object_type].meshes_offset;
            uint base_mesh_id = OBJECT_MESH_LIST[mesh_offset];
            [unroll]
            for (uint lod = 0; lod < 5; ++lod) {
                uint c = counts[lod];
                if (c != 0) {
                    InterlockedAdd(COUNT_PER_MESH[base_mesh_id + lod], c);
                }
            }
        }
    }
    // OBJECT PASS
    else if (PUSH_CONSTANTS.mode == 1) {
        // workgroup setup
        if (gtid.x < 25) // init to zero
            counts[gtid.x] = 0;
        GroupMemoryBarrierWithGroupSync();
        // same fetches for whole workgroup
        uint stride, mesh_count, object_count, chunk_count;
        OBJECTS.GetDimensions(object_count, stride);
        MESH_INFO.GetDimensions(mesh_count, stride);
        CHUNKS.GetDimensions(chunk_count, stride);
        static const uint max_chunk_tiles = 6400;
        uint chunk_id = VISIBLE_CHUNK_IDS[gid.x]; // id in global chunk buffer

        // fetches per object
        uint id_within_chunk = gtid.x;
        uint object_id = (chunk_id * 64) + id_within_chunk; // id in global objects buffer
        if (object_id >= object_count) return; // early out

        // unpack object position and move to camera space
        float3 position = float3(0);
        uint object_type = CHUNKS[chunk_id].object_type;
        if (object_type == 0) {
            // terrain: assuming each object is 128m wide
            float3 chunk_position = float3(int(chunk_id / 80) * 1024 - 40960 + 512, 0, int(chunk_id % 80) * 1024 - 40960 + 512);
            float3 object_position = float3(int(id_within_chunk / 8) * 128 - 512 + 64, 0, int(id_within_chunk % 8) * 128 - 512 + 64);
            object_position += chunk_position;
            float2 uv0 = (object_position.xz + (128.0f * 8.0f * 80.0f * 0.5f)) / (128.0f * 8.0f * 80.0f);
            uv0 = float2(1,1) + float2(uv0.x, -uv0.y);
            float height = TEXTURES[2].SampleLevel(uv0,0).x * 1000.0f;
            object_position.y += height;
            position = object_position - (camera_position / 10); // in meters
        } else if (object_type == 1) {
            uint2 object = OBJECTS[object_id]; // 8 bytes
            float3 object_position = float3(((int16_t) (object.x & 0xFFFF)), ((int16_t)((object.x >> 16) & 0xFFFF)), ((int16_t) (object.y & 0xFFFF)));
            float2 uv0 = ((object_position.xz / 10) + (128.0f * 8.0f * 80.0f * 0.5f)) / (128.0f * 8.0f * 80.0f);
            uv0 = float2(1,1) + float2(uv0.x, -uv0.y);
            float height = TEXTURES[2].SampleLevel(uv0, 0).x * 10000.0f;
            object_position.y += height;
            position = object_position - camera_position;
        }
        float position_yaw_z =  camera_yaw_sin * position.x + camera_yaw_cos * position.z; // apply yaw to Z
        float3 view = float3(
             camera_yaw_cos   * position.x - camera_yaw_sin   * position.z, // apply yaw to X
             camera_pitch_cos * position.y + camera_pitch_sin * position_yaw_z, // apply pitch to Y (using new z)
            -camera_pitch_sin * position.y + camera_pitch_cos * position_yaw_z // apply pitch to Z (using new z)
        );

        uint mesh_offset = OBJECT_METADATA[object_type].meshes_offset;
        uint base_mesh_id = OBJECT_MESH_LIST[mesh_offset];
        float distance = dot(position,position) / 500.0f;
        float radius = OBJECT_METADATA[object_type].radius;
        uint lod_level = choose_lod_step(distance, radius);
        if (object_type == 0) lod_level = 3;
        bool outside_frustrum = (abs(view.x) - radius) > (view.z * tanHalfFovX) || (abs(view.y) - radius) > (view.z * tanHalfFovY);
        bool valid = true;
        if (outside_frustrum) valid = false;

        if (valid) {
            uint slot; InterlockedAdd(counts[lod_level], 1, slot);
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
                    InterlockedAdd(COUNT_PER_MESH[base_mesh_id + lod], c);
                }
            }

            if (chunk_id >= 6400) {
                // heads: 5 LODs (offset +40)
                [unroll]
                for (uint lod = 0; lod < 5; ++lod) {
                    uint c = counts[5 + lod];
                    if (c != 0) {
                        InterlockedAdd(COUNT_PER_MESH[base_mesh_id + 40 + lod], c);
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
        INDIRECT_WORKGROUPS[0].group_count_x += 100; // add 6400 threads for possible chunk-to-object tiles on the map
        // INDIRECT_WORKGROUPS[0].group_count_x = (VISIBLE_OBJECT_COUNT[0] + 63) / 64;
        // INDIRECT_WORKGROUPS[0].group_count_y = 1;
        // INDIRECT_WORKGROUPS[0].group_count_z = 1;
        
        return;
    }
    // scatter
    else if (PUSH_CONSTANTS.mode == 3) {
        uint stride, mesh_count;
        MESH_INFO.GetDimensions(mesh_count, stride);

        bool is_chunk_object = gid.x < 100;
        uint chunk_id = !is_chunk_object ? VISIBLE_CHUNK_IDS[gid.x - 100] : tid.x; // id in global chunk buffer, minus the 100 we added for the 6400 possible chunks tiles
        uint id_within_chunk = gtid.x;
        uint object_id = (chunk_id * 64) + id_within_chunk; // id in global objects buffer

        // WRITE PACKED INSTANCE
        // unpack object position and move to camera space
        float3 chunk_position = float3(0);
        float3 position = float3(0);
        float object_yaw_cos = 1;
        float object_yaw_sin = 0;
        uint object_type = CHUNKS[chunk_id].object_type;
        if (is_chunk_object) {
            object_id = chunk_id * 64;
            chunk_position = float3(int(chunk_id / 80) * 1024 - 40960 + 512, 0, int(chunk_id % 80) * 1024 - 40960 + 512);
            float2 uv0 = (chunk_position.xz + (128.0f * 8.0f * 80.0f * 0.5f)) / (128.0f * 8.0f * 80.0f);
            uv0 = float2(1,1) + float2(uv0.x, -uv0.y);
            float height = TEXTURES[2].SampleLevel(uv0,0).x * 1000.0f;
            chunk_position.y += height;
            position = chunk_position - (camera_position / 10); // in meters
        } else if (object_type == 0) {
            chunk_position = float3(int(chunk_id / 80) * 1024 - 40960 + 512, 0, int(chunk_id % 80) * 1024 - 40960 + 512);
            float3 object_position = float3(int(id_within_chunk / 8) * 128 - 512 + 64, 0, int(id_within_chunk % 8) * 128 - 512 + 64);
            object_position += chunk_position;
            float2 uv0 = (object_position.xz + (128.0f * 8.0f * 80.0f * 0.5f)) / (128.0f * 8.0f * 80.0f);
            uv0 = float2(1,1) + float2(uv0.x, -uv0.y);
            float height = TEXTURES[2].SampleLevel(uv0,0).x * 1000.0f;
            object_position.y += height;
            position = object_position - (camera_position / 10); // in meters
        } else if (object_type == 1) {
            uint2 object = OBJECTS[object_id]; // 8 bytes
            float3 object_position = float3(((int16_t) (object.x & 0xFFFF)), ((int16_t)((object.x >> 16) & 0xFFFF)), ((int16_t) (object.y & 0xFFFF)));
            float2 uv0 = ((object_position.xz / 10) + (128.0f * 8.0f * 80.0f * 0.5f)) / (128.0f * 8.0f * 80.0f);
            uv0 = float2(1,1) + float2(uv0.x, -uv0.y);
            float height = TEXTURES[2].SampleLevel(uv0,0).x * 10000.0f;
            object_position.y += height;
            position = object_position - camera_position;
        }

        float position_yaw_z = camera_yaw_sin * position.x + camera_yaw_cos * position.z; // apply yaw to Z
        float3 view = float3(
             camera_yaw_cos   * position.x - camera_yaw_sin   * position.z, // apply yaw to X
             camera_pitch_cos * position.y + camera_pitch_sin * position_yaw_z, // apply pitch to Y (using new z)
            -camera_pitch_sin * position.y + camera_pitch_cos * position_yaw_z // apply pitch to Z (using new z)
        );

        // pick lod level
        uint mesh_offset = OBJECT_METADATA[object_type].meshes_offset;
        uint base_mesh_id = OBJECT_MESH_LIST[mesh_offset];
        float distance = dot(position,position) / 500.0f;
        float radius = OBJECT_METADATA[object_type].radius;
        if (is_chunk_object) radius = radius * 20;
        uint lod_level = choose_lod_step(distance, radius);
        if (object_type == 0) lod_level = 3;
        uint mesh_id = base_mesh_id + lod_level;
        
        bool outside_frustrum = (abs(view.x) - radius) > (view.z * tanHalfFovX) || (abs(view.y) - radius) > (view.z * tanHalfFovY);
        bool valid = true;
        if (outside_frustrum) valid = false;

        if (is_chunk_object && !(chunk_position.x < -2048 || chunk_position.x >= 2048 || chunk_position.z < -2048 || chunk_position.z >= 2048)) valid = false;

        if (valid) {
            // Grab slot via atomic write head on the draw call
            uint slot; InterlockedAdd(DRAW_CALLS[mesh_id].instance_count, 1, slot);
            const uint dst = OFFSET_PER_MESH[mesh_id] + slot;
            if (is_chunk_object) {
                RENDERED_INSTANCES[dst] = instance(chunk_id + 1000000, 0);
            } else if (object_type == 0) {
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
}

struct vs_out {
    float4 pos    : SV_Position;                                           // clip-space position
    float2 uv     : TEXCOORD0;                                             // texture coordinates
    float3 normal : TEXCOORD1;                                             // surface normal
    float3 p_view : TEXCOORD2;
    uint object_id : TEXCOORD3;
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
    if (PUSH_CONSTANTS.mode == 1 || PUSH_CONSTANTS.mode == 5) { // sky and ui both
        vs_out o;
        float2 p;
        if (VERTEX_ID == 1) p = float2(-1.0,  3.0); // was (-1,-1)
        if (VERTEX_ID == 0) p = float2( 3.0, -1.0);
        if (VERTEX_ID == 2) p = float2(-1.0, -1.0); // was (-1, 3)
        o.pos = float4(p, 0.0000001, 1.0); // 1.0 / 1.0 -> drawn behind everything else
        o.normal = float3(0.3, 0.5, 1.0);
        o.object_id = 0;
        float2 ndc = p * 0.5f + 0.5f;
        o.uv = float2(ndc.x, 1.0 - ndc.y);
        if (PUSH_CONSTANTS.mode == 5) o.pos.z = 1.0; // ui on top of everything
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
        // terrain
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
        uint vps = MAX_VERTICES >> DRAW_ID;                 // vertices per side
        float step_cm = TILE_SIZE_CM / float(vps - 1);      // neighbor vertex spacing (cm)
        if (is_chunk_object) step_cm *= 8;
        float2 uv0 = (object_position.xz + (vertex.xz / 100.0f) + (128.0f * 8.0f * 80.0f * 0.5f)) / (128.0f * 8.0f * 80.0f);
        uv0 = float2(1,1) + float2(uv0.x, -uv0.y);
        float hC = TEXTURES[2].SampleLevel(uv0, 0).x * 100000.0f;
        vertex.y += hC;
        normal.x = hC;
        normal.y = step_cm;
        uv = uv0;
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
        uint packed_normal_next = NORMALS[VERTEX_ID + base_vertex_next];
        normal = float3(
            float(((int)( packed_normal        & 0x3FF) << 22) >> 22) * divide_by_511,
            float(((int)((packed_normal >> 10) & 0x3FF) << 22) >> 22) * divide_by_511,
            float(((int)((packed_normal >> 20) & 0x3FF) << 22) >> 22) * divide_by_511
        );
        float3 normal_next = float3(
            float(((int)( packed_normal_next        & 0x3FF) << 22) >> 22) * divide_by_511,
            float(((int)((packed_normal_next >> 10) & 0x3FF) << 22) >> 22) * divide_by_511,
            float(((int)((packed_normal_next >> 20) & 0x3FF) << 22) >> 22) * divide_by_511
        );
        normal = normalize(lerp(normal, normal_next, alpha));
        // fetch and unpack uv (4 bytes)
        uint packed_uv = UVS[VERTEX_ID + base_vertex];
        uv = float2(
            (packed_uv & 0xFFFF) * (1.0/65535.0),
            (packed_uv >> 16)    * (1.0/65535.0)
        );
        // height
        float2 terrain_uv = ((object_position.xz / 10.0) + (128.0 * 8.0 * 80.0 / 2.0)) / (128.0 * 8.0 * 80.0);
        terrain_uv = float2(1,1) + float2(terrain_uv.x, -terrain_uv.y);
        float height = TEXTURES[2].SampleLevel(terrain_uv, 0).x;
        vertex.y += height * 100000.0f; // in cm
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
    o.p_view = float3(vx, vy, vz); // pass position to frag
    o.object_id = INSTANCE.object_id;
    return o;
}

struct ps_out {
    float4 color : SV_Target0; // normal color
    uint   id    : SV_Target1; // object ID
};
static const float3 LIGHT_DIR = normalize(float3(0.4, 0.2, 0.2)); // points *from surface to light*
static const float  AMBIENT   = 0.01;
[shader("fragment")]
ps_out fs_main(vs_out i, uint primID : SV_PrimitiveID) : SV_Target0 {
    ps_out o;
    o.id = i.object_id;
    if (PUSH_CONSTANTS.mode == 1) return ps_out(float4(i.normal, 1.0), i.object_id);
    if (PUSH_CONSTANTS.mode == 5) {
        bool inside =
            i.uv.x >= drag_rect_start_x &&
            i.uv.x <= drag_rect_end_x &&
            i.uv.y >= drag_rect_start_y &&
            i.uv.y <= drag_rect_end_y;
        if (!inside) discard;
        float px      = 1.0 / 1920.0; // or pass real width/height in
        float py      = 1.0 / 1080.0;
        float thick_x = 2.0 * px;
        float thick_y = 2.0 * py;
        bool on_border =
            i.uv.x < drag_rect_start_x + thick_x ||
            i.uv.x > drag_rect_end_x - thick_x ||
            i.uv.y < drag_rect_start_y + thick_y ||
            i.uv.y > drag_rect_end_y - thick_y;
        if (!on_border) discard;

        float4 border_color = float4(0.2, 0.8, 1.0, 0.8);  // strong

        return ps_out(border_color, i.object_id);
    }
    // terrain
    if (i.object_id > 1000000 || i.object_id < 6400 * 64) {
        float hC = TEXTURES[2].Sample(i.uv).x * 200000.0f;
        float step_cm = 20000.0f;
        float2 duv = float2(step_cm / 100.0f, 0);
        float2 dv  = float2(0, step_cm / 100.0f);
        duv /= (128.0f * 8.0f * 80.0f);
        dv  /= (128.0f * 8.0f * 80.0f);
        float hL = TEXTURES[2].Sample(i.uv - duv).x * 200000.0f;
        float hR = TEXTURES[2].Sample(i.uv + duv).x * 200000.0f;
        float hD = TEXTURES[2].Sample(i.uv - dv).x * 200000.0f;
        float hU = TEXTURES[2].Sample(i.uv + dv).x * 200000.0f;
        // --- build normal from grid-space slopes ---
        float3 dPdx = float3(2.0f * step_cm, (hR - hL), 0.0f);
        float3 dPdz = float3(0.0f, (hU - hD), 2.0f * step_cm);
        i.normal = normalize(cross(dPdz, dPdx));
    }
    float3 N = normalize(i.normal);
    float ndl = saturate(dot(N, LIGHT_DIR));
    float shade = AMBIENT + (1.0 - AMBIENT) * ndl;
    // // debug draw triangles as distinct colors
    float3 color = frac(sin((primID+43) * 43.5453) * float3(23.1407, 85.928, 43.533));
    if(color.x+color.y+color.z < 0.1) color = float3(float(primID % 32) / 32, float((primID + 38) % 64) / 64, float((primID + 17) % 32) / 32);
    // return float4(color, 1.0);
    // float4 color = TEXTURES[0].Sample(i.uv);
    float4 output = TEXTURES[1].Sample(i.uv);
    o.color = output;
    o.color.xyz += color / 10;
    o.color *= shade;
    if (i.object_id == selected_object_id) {
        o.color.xyz += float3(0.2);
    }
    return o;
}
