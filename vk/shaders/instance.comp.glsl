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

void main() {
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= INSTANCES.length()) return;

    uint slot = atomicAdd(COUNTERS.instance_count, 1u);
    VISIBLE[slot] = gid;
}
