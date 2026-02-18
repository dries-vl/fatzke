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

// movement of units: just move to next position
// -> do local per-entity movement cheap collision detection on 4m blocks and slide along to avoid passing through
// -> to always be able to handle stairs we can do mini local 4m block pathing per entity to avoid permanent stuck
// ----> because whole-unit movement only moves 1-2 blocks at a time as well, it will always be cheap
// combat movement: also just move to next position, and fight when collide
// -> force stay close to side neighbors to avoid blurring the front line in combat
// don't automatically envelop smaller enemy units, force closeness to lateral right-hand neighbor

// attrition: when death -> search neighborhood and shift the lowest priority entity to your position -> then do same for that one to fill up -> until no lower guy is found
// don't reform the formation so keep attrition-deformed positions until explicit reform command or far away move order

// conceptually a small amount of meta-object types; inf unit, cav unit, plane, ... -> animations based on this type
// -> 'animation set' of a mesh, a mesh can only be used for a given animation set
// let's now just assume there's two animations for units, none for planes
// todo: hardcode 0 animations for planes, hardcode 8 animations for everything else for easy offsetting no fetch needed
struct object_metadata { uint meshes_offset, mesh_count; float radius;};
[UnscopedEnum] enum object_type: int {TERRAIN, UNIT, BUILDING, VEGETATION};
struct chunk { object_type object_type;};
struct object { int16_t x, y, z; uint8_t cos, sin;};
struct mdi {uint index_count, instance_count, first_index, base_vertex, first_instance;};
struct di {uint group_count_x, group_count_y, group_count_z;};
struct instance { uint object_id, animation;};
struct unit { uint16_t x, y, next_x, next_y;};

// functional data
[[vk::binding(17,0)]] RWStructuredBuffer<unit> UNITS;    // size is # of chunks
[[vk::binding(0,0)]]  StructuredBuffer<chunk>  CHUNKS;    // size is # of chunks
[[vk::binding(4,0)]]  RWStructuredBuffer<object>  OBJECTS;   // size is total # of objects in scene
[[vk::binding(16,0)]] RWStructuredBuffer<uint> BUCKETS; // 8mb for 131k buckets, need about 4x the amount of used 2mx2m tiles, appr. 2x the amount of units, this should work up to 60k units
// base rendering data
[[vk::binding(14,0)]] StructuredBuffer<uint>            OBJECT_MESH_LIST; // for each object type the mesh ids that make up objects in order
[[vk::binding(15,0)]] StructuredBuffer<object_metadata> OBJECT_METADATA; // data about object type
[[vk::binding(9,0)]]  StructuredBuffer<mdi>             MESH_INFO; // render info for mesh (instance_count is 'max visible of this mesh in this scene')
[[vk::binding(11,0)]] StructuredBuffer<uint>   POSITIONS; // R10G10B10A2
[[vk::binding(12,0)]] StructuredBuffer<uint>   NORMALS;   // R10G10B10A2_SNORM
[[vk::binding(13,0)]] StructuredBuffer<uint>   UVS;       // R16G16_UNORM
// intermediary buffers
[[vk::binding(1,0)]]  RWStructuredBuffer<uint>  VISIBLE_CHUNK_IDS;     // size is # of chunks
[[vk::binding(2,0)]]  RWStructuredBuffer<di>    INDIRECT_WORKGROUPS;   // append per 64 directly in chunk pass
[[vk::binding(7,0)]]  RWStructuredBuffer<uint>  VISIBLE_OBJECT_IDS;   // size is # of objects in visible chunks
[[vk::binding(3,0)]]  RWStructuredBuffer<uint>  VISIBLE_OBJECT_COUNT; // compute this in prefix pass
[[vk::binding(5,0)]]  RWStructuredBuffer<uint>  COUNT_PER_MESH;       // size is # of mesh types
[[vk::binding(6,0)]]  RWStructuredBuffer<uint>  OFFSET_PER_MESH;      // size is # of mesh types
// final resulting rendering data
[[vk::binding(10,0)]] RWStructuredBuffer<mdi>      DRAW_CALLS;         // MDI, one struct per mesh type, index is mesh id
[[vk::binding(8,0)]]  RWStructuredBuffer<instance> RENDERED_INSTANCES; // size is max # of visible mesh instances
// uniforms
[[vk::binding(18,0)]]
cbuffer uniforms {
    float3 camera_position;
    float  camera_pitch_sin, camera_pitch_cos;
    float  camera_yaw_sin, camera_yaw_cos;
    float  time; // seconds
    uint   selected_object_id;
    float  drag_rect_start_x, drag_rect_start_y, drag_rect_end_x, drag_rect_end_y;
};
[[vk::binding(19, 0)]] Sampler2D TEXTURES[64];
[UnscopedEnum] enum mode: int {CHUNK_PASS, OBJECT_PASS, PREFIX_PASS, SCATTER_PASS, UI_MODE, FENCE_MODE, SEA_MODE, MESH_MODE, SKY_MODE};
// push constants
struct push_constants {
    mode mode;
};
[[vk::push_constant]] push_constants PUSH_CONSTANTS;

static const float d[4] = { 1024.0, 256.0, 64.0, 16.0 };
uint choose_lod_step(float distance, float radius) {
    if (radius <= 0) radius = 10.0;
    distance = distance / radius;
    if (distance >= d[0]) return 4;
    if (distance >= d[1]) return 3;
    if (distance >= d[2]) return 2;
    if (distance >= d[3]) return 1;
    return 0;
}

float terrain_height_at_uv(float2 uv) {
    return TEXTURES[5].SampleLevel(uv, 0).x * 100000.0f // height
    + (TEXTURES[4].SampleLevel(uv, 0).x - 0.25) * 1000.0f // noise
    - (TEXTURES[5].SampleLevel(uv, 1).b - 0.25) * 10000.0f; // coast
}
float chunk_height_at_uv(float2 uv) {
    return TEXTURES[2].SampleLevel(uv, 0).x * 20000.0f // height
    + (TEXTURES[4].SampleLevel(uv, 0).x - 0.25) * 250.0f // noise
    - (TEXTURES[2].SampleLevel(uv, 0).b - 0.4) * 10000.0f; // coast
}
float chunk_height_at_uv_fs(float2 uv) {
    return TEXTURES[2].Sample(uv).x * 20000.0f // height
    + (TEXTURES[4].Sample(uv).x - 0.25) * 250.0f // noise
    - (TEXTURES[2].Sample(uv).b - 0.9) * 10000.0f; // coast
}
float chunk_height_at_uv_px(int2 px) {
    return TEXTURES[2].Load(int3(px, 0)).x * 20000.0f // height
    + (TEXTURES[4].Load(int3(px, 0)).x - 0.25) * 250.0f // noise
    - (TEXTURES[2].Load(int3(px, 0)).b - 0.9) * 10000.0f; // coast
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
    // workgroup setup
    if (gtid.x < 25) // init to zero
        counts[gtid.x] = 0;
    GroupMemoryBarrierWithGroupSync();

    switch (PUSH_CONSTANTS.mode) {
    case CHUNK_PASS: {
        uint stride, chunk_count;
        CHUNKS.GetDimensions(chunk_count, stride);
        uint chunk_id = tid.x;
        bool real_chunk = chunk_id < chunk_count;
        if (real_chunk) {
            float3 delta_position;
            float3 chunk_position;
            bool terrain_chunk = chunk_id < 6400;
            if (terrain_chunk) {
                // assuming each tile is 1024m wide, and we use meters instead of dm now
                chunk_position = float3(int(chunk_id / 80) * 1024 - 40960 + 512, 0, int(chunk_id % 80) * 1024 - 40960 + 512);
                delta_position = chunk_position - (camera_position / 10); // in meters
            } else {
                object object = OBJECTS[chunk_id * 64];    // 8 bytes
                chunk_position = float3(object.x, object.y, object.z);
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
            // bool frustum = (abs(view.x) - radius) > (view.z * tanHalfFovX) || (abs(view.y) - radius) > (view.z * tanHalfFovY);
            // if (frustum && view.z > 0) valid = false;
            if (valid) {
                // distance
                float distance = dot(delta_position, delta_position) / 500.0f;
                // hardcode the inner 4km -> 4x4=16 chunks -> each 8x8=64 tiles -> each side 4x8=32 tiles x 8 verts = 256x256=65k vertices
                // each chunk there is 8 tiles x 8 vertices = 64x64=4096 vertices (lod1)
                // outer rim is remaining 6400-16 chunks -> ca. 6400 tiles -> each side 80 tiles x 4 verts = 320x320=102k vertices
                // 160x160 -> 160x160=25600 tiles -> each side 160 tiles x 4 verts = 640x640=409k vertices
                // 25600 tiles / 64 = 400 chunks, at 1 quad of 4 vert each -> 25600 x 4 = 102k vertices
                // each chunk there is 4x4=16 vertices (lod5)
                if (chunk_id < 6400 && (chunk_position.x < -2048 || chunk_position.x >= 2048 || chunk_position.z < -2048 || chunk_position.z >= 2048)) {
                // if (chunk_id < 6400) {
                //if (chunk_id < 6400 && distance > d[0] + 50000) {
                    // if really far, just directly write one visible instance, instead of 64 tile objects
                    uint stride; uint mesh_count;
                    MESH_INFO.GetDimensions(mesh_count, stride);
                    int object_type = (int) CHUNKS[chunk_id].object_type;
                    // uint lod_level = choose_lod_step(distance, OBJECT_METADATA[object_type].radius * 20);
                    uint lod_level = 1;
                    uint slot; InterlockedAdd(counts[lod_level], 1, slot);
                } else {
                    // else spawn a workgroup of 64 objects
                    uint slot; InterlockedAdd(INDIRECT_WORKGROUPS[0].group_count_x, 1, slot);
                    INDIRECT_WORKGROUPS[0].group_count_y = 1;
                    INDIRECT_WORKGROUPS[0].group_count_z = 1;
                    VISIBLE_CHUNK_IDS[slot] = chunk_id;
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
        if (real_chunk && gtid.x == 0) {
            // bodies: 5 LODs
            int object_type = (int) CHUNKS[chunk_id].object_type;
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
    } break;
    case OBJECT_PASS: 
    case SCATTER_PASS: {
        // same fetches for whole workgroup
        uint stride, mesh_count, object_count;
        OBJECTS.GetDimensions(object_count, stride);
        MESH_INFO.GetDimensions(mesh_count, stride);

        bool is_chunk_object = PUSH_CONSTANTS.mode == SCATTER_PASS
            ? gid.x < 100 // first 6400 threads are chunk objects in scatter pass
            : false;

        uint chunk_id = !is_chunk_object 
            ? VISIBLE_CHUNK_IDS[gid.x - 100] // offset the 100 added workgroups
            : tid.x; // global thread is id, because first 6400 threads

        // fetches per object
        uint id_within_chunk = gtid.x;
        uint object_id = (chunk_id * 64) + id_within_chunk; // id in global objects buffer
        if (object_id >= object_count) return; // early out

        // unpack object position and move to camera space
        object_type object_type = CHUNKS[chunk_id].object_type;
        float3 position = float3(0);
        float3 chunk_position = float3(0);
        if (is_chunk_object) {
            object_id = chunk_id * 64;
            chunk_position = float3(int(chunk_id / 80) * 1024 - 40960 + 512, 0, int(chunk_id % 80) * 1024 - 40960 + 512);
            float2 uv0 = (chunk_position.xz + (TOTAL_WIDTH * 0.5f)) / (TOTAL_WIDTH);
            uv0 = float2(0,1) - float2(-uv0.x, uv0.y);
            float height = TEXTURES[2].SampleLevel(uv0, 0).x * 1000.0f
            + (TEXTURES[4].SampleLevel(uv0, 0).x - 0.25) * 10.0f
            - (TEXTURES[2].SampleLevel(uv0, 1).b - 0.5) * 50.0f;
            chunk_position.y += height;
            position = chunk_position - (camera_position / 10); // in meters
        } else if (object_type == TERRAIN) {
            chunk_position = float3(int(chunk_id / 80) * 1024 - 40960 + 512, 0, int(chunk_id % 80) * 1024 - 40960 + 512);
            float3 object_position = float3(int(id_within_chunk / 8) * 128 - 512 + 64, 0, int(id_within_chunk % 8) * 128 - 512 + 64);
            object_position += chunk_position;
            float2 uv0 = (object_position.xz + (TOTAL_WIDTH * 0.5f)) / (TOTAL_WIDTH);
            uv0 = float2(0,1) - float2(-uv0.x, uv0.y);
            float height = TEXTURES[5].SampleLevel(uv0, 0).x * 1000.0f
            + (TEXTURES[4].SampleLevel(uv0, 0).x - 0.25) * 10.0f
            - (TEXTURES[5].SampleLevel(uv0, 1).b - 0.5) * 50.0f;
            object_position.y += height;
            position = object_position - (camera_position / 10); // in meters
        } else if (object_type == UNIT) {
            object object = OBJECTS[object_id]; // 8 bytes
            unit UNIT = UNITS[0];
            int rank = object_id % 64;
            float2 formation = float2(rank % 8, rank / 8) * 10.0f;
            float2 object_position = formation;
            float2 uv0 = ((object_position.xy / 10) + (128.0f * 8.0f * 4.0f * 0.5f)) / (128.0f * 8.0f * 4.0f);
            uv0 = float2(0,1) - float2(-uv0.x, uv0.y);
            float height = terrain_height_at_uv(uv0) / 10;
            position = float3(object_position.x, height, object_position.y) - camera_position;
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
        if (object_type == TERRAIN) lod_level = 1;
        if (is_chunk_object) lod_level = 1;
        bool outside_frustrum = (abs(view.x) - radius) > (view.z * tanHalfFovX) || (abs(view.y) - radius) > (view.z * tanHalfFovY);
        bool valid = true;
        // if (outside_frustrum) valid = false;
        if (is_chunk_object && !(chunk_position.x < -2048 || chunk_position.x >= 2048 || chunk_position.z < -2048 || chunk_position.z >= 2048)) valid = false;

        if (PUSH_CONSTANTS.mode == OBJECT_PASS) {
            // append to count of the lod level of this object
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
                        // assumes whole workgroup is the same object type
                        InterlockedAdd(COUNT_PER_MESH[base_mesh_id + lod], c);
                    }
                }
                if (chunk_id >= 6400) {
                    // heads: 5 LODs (offset +40)
                    [unroll]
                    for (uint lod = 0; lod < 5; ++lod) {
                        uint c = counts[5 + lod];
                        if (c != 0) {
                            // 8 frames in animation, each frame has 5 lod levels -> 40 meshes further to next
                            InterlockedAdd(COUNT_PER_MESH[base_mesh_id + 40 + lod], c);
                        }
                    }
                }
            }
        } else if (PUSH_CONSTANTS.mode == SCATTER_PASS) {
            // append to the contiguous instance list
            if (valid) {
                uint mesh_id = base_mesh_id + lod_level;
                // Grab slot via atomic write head on the draw call
                uint slot; InterlockedAdd(DRAW_CALLS[mesh_id].instance_count, 1, slot);
                const uint dst = OFFSET_PER_MESH[mesh_id] + slot;
                if (is_chunk_object) {
                    RENDERED_INSTANCES[dst] = instance(chunk_id + 1000000, 0);
                } else if (object_type == TERRAIN) {
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
    } break;
    case PREFIX_PASS: {
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
    } break;
    }
}

// FENCE
static const float FENCE_S = 2048; // meters
static const float FENCE_H = 200;   // meters
static const float SEA_S = 81920 / 2;   // meters
static const float3 SEA_V[4] = {
    float3(-SEA_S, 0.0, -SEA_S),
    float3( SEA_S, 0.0, -SEA_S),
    float3( SEA_S, 0.0,  SEA_S),
    float3(-SEA_S, 0.0,  SEA_S),
};
static const uint SEA_I[6] = {
    0,1,2, 2,3,0
};
static const float3 FENCE_V[8] = {
    float3(-FENCE_S, 0.0, -FENCE_S),
    float3( FENCE_S, 0.0, -FENCE_S),
    float3( FENCE_S, 0.0,  FENCE_S),
    float3(-FENCE_S, 0.0,  FENCE_S),
    float3(-FENCE_S, FENCE_H, -FENCE_S),
    float3( FENCE_S, FENCE_H, -FENCE_S),
    float3( FENCE_S, FENCE_H,  FENCE_S),
    float3(-FENCE_S, FENCE_H,  FENCE_S),
};
static const uint FENCE_I[24] = {
    1,0,5, 5,0,4,   // south
    2,1,6, 6,1,5,   // east
    3,2,7, 7,2,6,   // north
    0,3,4, 4,3,7    // west
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

static const float TILE_WIDTH = 128.0;
static const float TILES_PER_CHUNK = 8.0; // width
static const float CHUNKS_PLAYABLE = 4.0; // width
static const float CHUNKS_TOTAL = 80.0; // width
static const float PLAYABLE_WIDTH = TILE_WIDTH * TILES_PER_CHUNK * CHUNKS_PLAYABLE;
static const float TOTAL_WIDTH = TILE_WIDTH * TILES_PER_CHUNK * CHUNKS_TOTAL;
static const float VERTEX_WIDTH = TILE_WIDTH / float((MAX_VERTICES >> 2) - 1); // >> 2 because terrain lod is always 2
float2 terrain_uv_at(float2 worldXZ_m) {
    float2 uv = (worldXZ_m + (PLAYABLE_WIDTH / 2)) / PLAYABLE_WIDTH;
    uv = float2(0,1) - float2(-uv.x, uv.y);
    return uv;
}
float2 chunk_uv_at(float2 worldXZ_m) {
    float2 uv = (worldXZ_m + (TOTAL_WIDTH * 0.5f)) / (TOTAL_WIDTH);
    uv = float2(0, 1) - float2(-uv.x, uv.y);
    return uv;
}
// returns UVs of the 3 terrain vertices of the triangle under the point
float terrain_height_at(float2 worldXZ_m) {
    float2 p = worldXZ_m + (PLAYABLE_WIDTH / 2);            // [0..4096)
    int2 tile = int2(floor(p / TILE_WIDTH));
    tile = clamp(tile, int2(0,0), int2(31,31));
    // tile origin in world coords (meters)
    float2 tileOrigin = float2(tile) * TILE_WIDTH - (PLAYABLE_WIDTH / 2);
    // tile-local coordinate [0..128)
    float2 local = worldXZ_m - tileOrigin;
    // --- snap to fixed vertex cell on the 32x32 lattice ---
    float2 cellF = local / VERTEX_WIDTH;               // 0..31 in ideal range
    int2 cell = int2(floor(cellF));
    cell = clamp(cell, int2(0,0), int2(MAX_VERTICES-2, MAX_VERTICES-2));
    float2 f = frac(cellF);                      // position inside cell [0..1)
    // 4 surrounding lattice vertices in world meters
    float2 p00 = tileOrigin + float2(cell) * VERTEX_WIDTH;
    float2 p10 = p00 + float2(VERTEX_WIDTH, 0);
    float2 p01 = p00 + float2(0, VERTEX_WIDTH);
    float2 p11 = p00 + float2(VERTEX_WIDTH, VERTEX_WIDTH);
    float2 uvA, uvB, uvC;
    // choose which tri (diagonal split consistent with a regular grid)
    if (f.x + f.y <= 1.0) {
        uvA = terrain_uv_at(p00);
        uvB = terrain_uv_at(p10);
        uvC = terrain_uv_at(p01);
        // weights would be: (1-fx-fy, fx, fy) for (A,B,C)
    } else {
        uvA = terrain_uv_at(p11);
        uvB = terrain_uv_at(p01);
        uvC = terrain_uv_at(p10);
        // weights would be: (fx+fy-1, 1-fx, 1-fy) for (A,B,C) in (A,B,C) order
    }
    // sample heights at the three terrain vertices
    float hA = terrain_height_at_uv(uvA);
    float hB = terrain_height_at_uv(uvB);
    float hC = terrain_height_at_uv(uvC);
    // barycentric interpolation (same split rule as helper)
    float height;
    if (f.x + f.y <= 1.0) {
        // tri (A,B,C) = (00,10,01)
        height = (1.0 - f.x - f.y) * hA
               + f.x * hB
               + f.y * hC;
    } else {
        // tri (A,B,C) = (11,01,10)
        height = (f.x + f.y - 1.0) * hA
               + (1.0 - f.x) * hB
               + (1.0 - f.y) * hC;
    }
    return height;
}

struct vs_out {
    float4 pos    : SV_Position;                                           // clip-space position
    float2 uv     : TEXCOORD0;                                             // texture coordinates
    float3 normal : TEXCOORD1;                                             // surface normal
    bool is_chunk : TEXCOORD2;
    uint object_id : TEXCOORD3;
    float h : TEXCOORD4;
};

[shader("vertex")]
vs_out vs_main(uint VERTEX_ID : SV_VertexID, [[vk::location(0)]] instance INSTANCE, uint DRAW_ID : SV_DrawIndex) {
    // SKY path: fullscreen triangle
    if (PUSH_CONSTANTS.mode == SKY_MODE || PUSH_CONSTANTS.mode == UI_MODE) { // sky and ui both
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
        if (PUSH_CONSTANTS.mode == UI_MODE) o.pos.z = 1.0; // ui on top of everything
        return o;
    } else if (PUSH_CONSTANTS.mode == FENCE_MODE || PUSH_CONSTANTS.mode == SEA_MODE) { // fence and sea
        float3 p;
        if (PUSH_CONSTANTS.mode == FENCE_MODE)
            p = FENCE_V[FENCE_I[VERTEX_ID]];
        else
            p = SEA_V[SEA_I[VERTEX_ID]];
        float3 d = p - (camera_position / 10.0);
        // yaw + pitch (same math as rest of engine)
        float zYaw = camera_yaw_sin * d.x + camera_yaw_cos * d.z;
        float3 vM = float3(
            camera_yaw_cos   * d.x - camera_yaw_sin   * d.z,
            camera_pitch_cos * d.y + camera_pitch_sin * zYaw,
           -camera_pitch_sin * d.y + camera_pitch_cos * zYaw
        );
        // meters -> cm for projection
        float vx = vM.x * 100.0;
        float vy = vM.y * 100.0;
        float vz = vM.z * 100.0;
        vs_out o;
        o.pos = float4(proj_scale_x * vx, proj_scale_y * vy, near_plane, vz);
        o.uv = 0;
        if (PUSH_CONSTANTS.mode == SEA_MODE) {
            o.uv = (p.xz + SEA_S) * (1.0f / (2.0f * SEA_S));
            o.uv.y = 1.0f - o.uv.y;
        }
        o.normal = 0;
        o.object_id = 900000000u; // optional pick id
        return o;
    }

    // fetch and unpack visible instance data, convert dm to cm (12 bytes)
    float object_yaw_cos = 1.0;
    float object_yaw_sin = 0.0;
    float3 position;
    float3 vertex;
    float3 normal;
    float h = 0.0;
    float2 uv;

    // draw id is essentially just the mesh id in this case
    uint object_id = INSTANCE.object_id;
    bool is_chunk = object_id > 1000000;
    bool is_terrain = DRAW_ID < 5; // first five draw ids are the five terrain lod levels
    if (is_terrain) {
        object_id = is_chunk ? object_id - 1000000 : object_id;
        uint chunk_id = is_chunk ? object_id : object_id / 64;
        float3 chunk_position = float3(int(chunk_id / 80) * 1024 - 40960 + 512, 0, int(chunk_id % 80) * 1024 - 40960 + 512);
        float2 world_pos;
        if (is_chunk) {
            position = 10 * (chunk_position - (camera_position / 10)); // in meters
            vertex = 8 * plane_position(VERTEX_ID, DRAW_ID);           // in cm, times 8 because a chunk is 8x wider than a terrain tile
            world_pos = chunk_position.xz + (vertex.xz / 100.0f);
            uv = chunk_uv_at(world_pos);
            h = chunk_height_at_uv(uv);
            vertex.y += h;
        } else {
            uint id_within_chunk = object_id - (chunk_id * 64);
            float3 object_position = float3(int(id_within_chunk / 8) * 128 - 512 + 64, 0, int(id_within_chunk % 8) * 128 - 512 + 64);
            object_position += chunk_position;
            position = 10 * (object_position - (camera_position / 10)); // in meters
            vertex = plane_position(VERTEX_ID, DRAW_ID);                // in cm
            world_pos = object_position.xz + (vertex.xz / 100.0f);
            uv = chunk_uv_at(world_pos);
            h = chunk_height_at_uv(uv);
            vertex.y += h;
        }
        // normal
        float step_m = 100.0f; float step_cm = step_m * 100.0;
        float hL = chunk_height_at_uv(chunk_uv_at(world_pos + float2(-step_m, 0)));
        float hR = chunk_height_at_uv(chunk_uv_at(world_pos + float2( step_m, 0)));
        float hD = chunk_height_at_uv(chunk_uv_at(world_pos + float2(0, -step_m)));
        float hU = chunk_height_at_uv(chunk_uv_at(world_pos + float2(0,  step_m)));
        float3 dPdx = float3(2.0f * step_cm, hR - hL, 0.0f);
        float3 dPdz = float3(0.0f, hU - hD, 2.0f * step_cm);
        normal = normalize(cross(dPdz, dPdx));
    } else {
        // unit
        object object = OBJECTS[object_id];    // 8 bytes
        unit UNIT = UNITS[0];
        int rank = object_id % 64;
        float2 formation = float2(rank % 8, rank / 8) * /*spacing*/ 10.0f;
        float3 object_position = float3(formation.x, 0, formation.y);
        position = object_position - camera_position;
        float object_yaw_cos = float(object.cos) * divide_by_127;
        float object_yaw_sin = float(object.sin) * divide_by_127;
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
        float2 worldXZ_m = object_position.xz / 10.0f;
        vertex.y += terrain_height_at(worldXZ_m);
    }

    // rotate position around y for the combined camera-instance yaw
    float cos_d = object_yaw_cos * camera_yaw_cos + object_yaw_sin * camera_yaw_sin; // cos(a-b)
    float sin_d = object_yaw_sin * camera_yaw_cos - object_yaw_cos * camera_yaw_sin; // sin(a-b)
    float x1 =  cos_d * vertex.x + sin_d * vertex.z;
    float z1 = -sin_d * vertex.x + cos_d * vertex.z;
    float y1 =  vertex.y;

    // rotate position around x for the camera pitch
    float y2 =  camera_pitch_cos * y1 + camera_pitch_sin * z1;
    float z2 = -camera_pitch_sin * y1 + camera_pitch_cos * z1;
    float x2 =  x1;

    // move position into the calculated instance-in-camera-space
    float position_yaw_z =  camera_yaw_sin * position.x + camera_yaw_cos * position.z; // apply yaw to Z
    float3 view = float3(
         camera_yaw_cos   * position.x - camera_yaw_sin   * position.z, // apply yaw to X
         camera_pitch_cos * position.y + camera_pitch_sin * position_yaw_z, // apply pitch to Y (using new z)
        -camera_pitch_sin * position.y + camera_pitch_cos * position_yaw_z // apply pitch to Z (using new z)
    );
    float vx = x2 + (10.0 * view.x); // times ten to go from m to cm
    float vy = y2 + (10.0 * view.y);
    float vz = z2 + (10.0 * view.z);

    // perspective projection (reverse dept)
    float cx = proj_scale_x * vx;
    float cy = proj_scale_y * vy;
    float cz = near_plane;
    float w = vz;

    vs_out o = vs_out(float4(-1));
    o.pos = float4(cx, cy, cz, w);
    o.normal = normal;
    o.uv = uv;
    o.is_chunk = is_chunk;
    o.object_id = INSTANCE.object_id;
    o.h = h;
    return o;
}

// idea: deterministic hash based on object id -> 0 or 1
// 1 means bigger -> farther means bigger, up to certain max size
// 0 means smaller -> farther means smaller, after certain distance ie. 50% smaller then cull -> no popping
// -> two for one at distance
// -> could scale up to four for one at even further distance
// -> or even 8 for one for smaller things, even 16 for one for grass
// -> extremely far away grass should just disappear entirely over distance
// -> should dynamically go from 1 to 2 for 1 to 4 for 1 to 8 for 1 to 16 for 1, etc. until basically none

struct ps_out {
    float4 color : SV_Target0; // normal color
    uint   id    : SV_Target1; // object ID
};
static const float3 LIGHT_DIR = normalize(float3(0.8, 0.1, 0.1)); // points *from surface to light*
static const float4 SKY_COLOR = float4(0.3, 0.5, 1.0, 1.0);
static const float4 SEA_COLOR = float4(0.0, 0.3, 0.6, 1.0);
static const float AMBIENT = 0.3;
[shader("fragment")]
ps_out fs_main(vs_out i, uint primID : SV_PrimitiveID) : SV_Target0 {
    ps_out o;
    o.id = i.object_id;
    if (PUSH_CONSTANTS.mode == SKY_MODE) o.color = SKY_COLOR;
    else if (PUSH_CONSTANTS.mode == UI_MODE) {
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

        o.color = border_color; }
    else if (PUSH_CONSTANTS.mode == FENCE_MODE) o.color = float4(0.0, 0.0, 0.0, 1.0);
    else if (PUSH_CONSTANTS.mode == SEA_MODE) o.color = SEA_COLOR;
    else if (PUSH_CONSTANTS.mode == MESH_MODE) {
        // debug draw triangles as distinct colors
        float3 color = frac(sin((primID+43) * 43.5453) * float3(23.1407, 85.928, 43.533));
        if(color.x+color.y+color.z < 0.1) color = float3(float(primID % 32) / 32, float((primID + 38) % 64) / 64, float((primID + 17) % 32) / 32);
        // float3 color = frac(sin((i.object_id+43) * 43.5453) * float3(23.1407, 85.928, 43.533));
        // if(color.x+color.y+color.z < 0.1) color = float3(float(i.object_id % 32) / 32, float((i.object_id + 38) % 64) / 64, float((i.object_id + 17) % 32) / 32);
        // return float4(color, 1.0);
        
        float slope = 1.0 - saturate(dot(i.normal, float3(0.0, 1.0, 0.0)));
        slope = pow(slope, 0.1);

        float4 map = TEXTURES[1].Sample(i.uv);
        // o.color.xyz = float3(0.5);
        o.color = map * (2 - (slope * 2));
        float h = chunk_height_at_uv_fs(i.uv) * 0.00033;
        if (h <= 2.0) {
            o.color = SEA_COLOR + (h * float4(0.1, 0.2, 0.3, 0.0));
            i.normal = 0;
        }
        // o.color.xyz = (color + o.color.xyz) / 2;
        // o.color *= 1.0 - saturate(i.h * 0.00025);
    }
    
    if (PUSH_CONSTANTS.mode == SEA_MODE) {
        float h = chunk_height_at_uv_fs(i.uv) * 0.00033;
        o.color.xyz += h * float3(0.1, 0.2, 0.3);
    }

    if (PUSH_CONSTANTS.mode == SEA_MODE || PUSH_CONSTANTS.mode == MESH_MODE) {
        // NORMAL
        // float hL = chunk_height_at_uv_px(int2(i.uv * 2048) + int2(-1, 0));
        // float hR = chunk_height_at_uv_px(int2(i.uv * 2048) + int2(1, 0));
        // float hD = chunk_height_at_uv_px(int2(i.uv * 2048) + int2(0, -1));
        // float hU = chunk_height_at_uv_px(int2(i.uv * 2048) + int2(0, 1));
        float px_size = 1.0f / 2048.0f;
        float hL = chunk_height_at_uv_fs(i.uv + float2(-px_size, 0));
        float hR = chunk_height_at_uv_fs(i.uv + float2(px_size, 0));
        float hD = chunk_height_at_uv_fs(i.uv + float2(0, -px_size));
        float hU = chunk_height_at_uv_fs(i.uv + float2(0, px_size));
        float3 dPdx = float3(2.0f * 5000, hR - hL, 0.0f);
        float3 dPdz = float3(0.0f, hU - hD, 2.0f * 5000);
        i.normal = normalize(cross(dPdz, dPdx));



        float ndl = saturate(dot(i.normal, LIGHT_DIR));
        float wrap = 0.05;
        float sun = saturate((ndl + wrap) / (1.0 + wrap));
        float contrast = 0.85;                     // 0..1
        sun = lerp(sun, sun * sun, contrast);      // pushes midtones down, keeps 0 and 1 fixed
        float sky = saturate(i.normal.z * 0.5 + 0.5);
        float fill = lerp(AMBIENT, 0.50, sky);
        fill = pow(fill, 2);
        float shade = fill + sun * 3.0; // intensity of the sunlight
        o.color *= shade;
    }

    if (PUSH_CONSTANTS.mode != SKY_MODE && i.object_id == selected_object_id) {
        o.color.xyz += float3(0.2);
    }
    float fog_t = saturate((max(0.0f,(1.0f/i.pos.w)*0.01f) - 40000.0f) * (1.0f/15000.0f));
    fog_t = fog_t * fog_t * (3.0f - 2.0f * fog_t); // smoothstep
    o.color = lerp(o.color, SKY_COLOR, fog_t);
    return o;
}
