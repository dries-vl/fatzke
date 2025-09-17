#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct Instance { vec2 offset; };

layout(std430, set = 0, binding = 0) readonly buffer INSTANCES_buf { Instance INSTANCES[]; };
layout(std430, set = 0, binding = 1)        buffer VISIBLE_buf   { uint     VISIBLE[];   };

// Matches VkDrawIndexedIndirectCommand layout
layout(std430, set = 0, binding = 2) buffer COUNTERS_buf {
    uint index_count;
    uint instance_count;
    uint first_index;
    int  base_vertex;
    uint first_instance;
} COUNTERS;

layout(std430, set = 0, binding = 3) readonly buffer VERTICES_buf { vec2 VERTICES[]; };
layout(std430, set = 0, binding = 4)        buffer VARYINGS_buf { vec2 VARYINGS[]; };
layout(std430, set = 0, binding = 5)        buffer INDICES_buf  { uint INDICES[];  };

shared vec2      s_vertices[64];
shared Instance  s_instance;
shared uint      s_instance_id;

void main() {
    uvec3 wg  = gl_WorkGroupID;
    uvec3 lid = gl_LocalInvocationID;

    // Only process existing instances
    if (wg.x >= COUNTERS.instance_count) return;

    // Load per-meshlet data into shared memory
    s_vertices[lid.x] = VERTICES[lid.x];  // (here we just grab first 64 slots)
    if (lid.x == 0u) {
        s_instance_id = VISIBLE[wg.x];
        s_instance    = INSTANCES[s_instance_id];
    }

    // ensure LDS is visible
    memoryBarrierShared();
    barrier();

    // Write 3 vertices
    if (lid.x >= 3u) return;
    uint base = s_instance_id * 3u;
    VARYINGS[base + lid.x] = s_vertices[lid.x] + s_instance.offset;

    // Thread 0 writes the triangle indices and bumps index_count by 3
    if (lid.x == 0u) {
        uint i_base = atomicAdd(COUNTERS.index_count, 3u);
        INDICES[i_base + 0u] = i_base + 0u;
        INDICES[i_base + 1u] = i_base + 1u;
        INDICES[i_base + 2u] = i_base + 2u;
    }
}
