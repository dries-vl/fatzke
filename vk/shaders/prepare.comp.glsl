#version 450
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

// same COUNTERS layout as above
layout(std430, set = 0, binding = 2) buffer COUNTERS_buf {
    uint index_count;
    uint instance_count;
    uint first_index;
    int  base_vertex;
    uint first_instance;
} COUNTERS;

// 3 uints: x, y, z for vkCmdDispatchIndirect
layout(std430, set = 0, binding = 6) buffer DISPATCH_buf { uint DISPATCH[3]; };

void main() {
    // x = number of visible meshlets (instances)
    DISPATCH[0] = COUNTERS.instance_count;
    DISPATCH[1] = 1u;
    DISPATCH[2] = 1u;
}
