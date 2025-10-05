struct Instance { 
    float2 offset;
};
struct Counters { 
    uint index_count;
    uint instance_count;
    uint first_index;
    int  base_vertex;
    uint first_instance;
};
[[vk::binding(0,0)]] StructuredBuffer<Instance>       INSTANCES;
[[vk::binding(1,0)]] RWStructuredBuffer<uint>         VISIBLE;
[[vk::binding(2,0)]] RWStructuredBuffer<Counters>     COUNTERS;
[[vk::binding(3,0)]] StructuredBuffer<float2>         VERTICES;
[[vk::binding(4,0)]] RWStructuredBuffer<float2>       VARYINGS;
[[vk::binding(5,0)]] RWStructuredBuffer<uint>         INDICES;
[[vk::binding(6,0)]] RWStructuredBuffer<uint>         DISPATCH;

// ---------- compute: cs_instance ----------
[shader("compute")] [numthreads(64,1,1)]
void cs_instance(uint3 tid : SV_DispatchThreadID) {
    uint gid = tid.x;

    uint numInst, stride;
    INSTANCES.GetDimensions(numInst, stride);
    if (gid >= numInst) return;

    uint slot;
    InterlockedAdd(COUNTERS[0].instance_count, 1, slot);
    VISIBLE[slot] = gid;
}

// ---------- compute: cs_prepare ----------
[shader("compute")] [numthreads(1,1,1)]
void cs_prepare() {
    DISPATCH[0] = COUNTERS[0].instance_count; // x
    DISPATCH[1] = 1;                          // y
    DISPATCH[2] = 1;                          // z
}

// ---------- compute: cs_meshlet ----------
groupshared float2   s_vertices[64];
groupshared Instance s_instance;
groupshared uint     s_instance_id;

[shader("compute")] [numthreads(64,1,1)]
void cs_meshlet(uint3 wg  : SV_GroupID, uint3 lid : SV_GroupThreadID) {
    if (wg.x >= COUNTERS[0].instance_count) return;

    s_vertices[lid.x] = VERTICES[lid.x];
    if (lid.x == 0)
    {
        s_instance_id = VISIBLE[wg.x];
        s_instance    = INSTANCES[s_instance_id];
    }

    GroupMemoryBarrierWithGroupSync();

    if (lid.x < 3)
    {
        uint base = s_instance_id * 3;
        VARYINGS[base + lid.x] = s_vertices[lid.x] + s_instance.offset;
    }

    if (lid.x == 0)
    {
        uint i_base;
        InterlockedAdd(COUNTERS[0].index_count, 3, i_base);
        INDICES[i_base + 0] = i_base + 0;
        INDICES[i_base + 1] = i_base + 1;
        INDICES[i_base + 2] = i_base + 2;
    }
}

// ---------- vertex ----------
struct VSOut { float4 pos : SV_Position; };

[shader("vertex")]
VSOut vs_main([[vk::location(0)]] float2 a_pos) {
    VSOut o;
    o.pos = float4(a_pos, 0.0, 1.0);
    return o;
}

// ---------- fragment ----------
[shader("fragment")]
float4 fs_main() : SV_Target0 {
    return float4(1.0, 0.4, 0.1, 1.0);
}