struct Counters {
    uint index_count;
    uint instance_count;
    uint first_index;
    int  base_vertex;
    uint first_instance;
};

static const uint kVertsPerMesh = 192;
static const uint kIdxPerMesh   = 1116;

// Simple right-handed look-at (eye at 0,2,-5 looking toward +Z, up +Y)
// Perspective: 60° vertical FOV, 16:9 aspect, near 0.1, far 100.0
static const float4x4 g_ViewProj = {
    // Column-major literal, i.e. four columns top-to-bottom
    0.9742786f,  0.0000000f,  0.0000000f,  0.0000000f,
    0.0000000f,  1.6081688f, -0.37176244f, -0.037176244f,
    0.0000000f,  0.64326752f,  0.92940610f,  0.092940610f,
    0.0000000f,  0.0000000f,   4.3905554f,   0.53905553f
};

float3 unpackA2B10G10R10_SNORM(uint p) {
    int r = (int)( p        & 0x3FF);
    int g = (int)((p >> 10) & 0x3FF);
    int b = (int)((p >> 20) & 0x3FF);
    r = (r << 22) >> 22; g = (g << 22) >> 22; b = (b << 22) >> 22;
    float s = 1.0f/511.0f;
    return float3(float(r)*s, float(g)*s, float(b)*s);
}
float2 decodeXZ(uint packed0) {
    int x16 = (int)(packed0 & 0xFFFF); x16 = (x16 << 16) >> 16;
    int z16 = (int)((packed0 >> 16) & 0xFFFF); z16 = (z16 << 16) >> 16;
    return float2(float(x16), float(z16));
}
float  decodeYaw(uint packed1)   { return (float(packed1 & 0xFFu) * (2.0f*3.14159265359f)) / 255.0f; }
uint   decodeMeshId(uint packed1){ return packed1 >> 8; }

[[vk::binding(0,0)]] StructuredBuffer<uint2>        INSTANCES;
[[vk::binding(1,0)]] RWStructuredBuffer<uint2>      VISIBLE_PACKED;
[[vk::binding(2,0)]] RWStructuredBuffer<Counters>   COUNTERS;
[[vk::binding(3,0)]] StructuredBuffer<uint>         POSITIONS; // todo: 8bit compressed texture
[[vk::binding(4,0)]] StructuredBuffer<uint>         NORMALS;   // todo: 8bit compressed texture

// ======= Compute: build visible (no culling yet) =======
[shader("compute")] [numthreads(64,1,1)]
void cs_build_visible(uint3 dtid : SV_DispatchThreadID) {
    uint gid = dtid.x;
    uint numInst, stride;
    INSTANCES.GetDimensions(numInst, stride);
    if (gid >= numInst) return;

    uint slot;
    InterlockedAdd(COUNTERS[0].instance_count, 1, slot);
    VISIBLE_PACKED[slot] = INSTANCES[gid];
}

// ======= Compute: prepare indirect draw =======
[shader("compute")] [numthreads(1,1,1)]
void cs_prepare_indirect() {
    COUNTERS[0].index_count    = kIdxPerMesh;
    COUNTERS[0].first_index    = 0;
    COUNTERS[0].base_vertex    = 0;
    COUNTERS[0].first_instance = 0;
}

// ======= Graphics =======
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// VS inputs:
// - vid: index from bound index buffer
// - packed: two uints for this visible instance (R32G32_UINT) at location=0
// - in_uv : per-vertex UV (R16G16_UNORM) at location=1
[shader("vertex")]
VSOut vs_main(uint vid : SV_VertexID, [[vk::location(0)]] uint2 packed, [[vk::location(1)]] float2 in_uv) {
    float2 xz  = decodeXZ(packed.x);
    float  yaw = decodeYaw(packed.y);
    uint   mesh= decodeMeshId(packed.y);

    uint vIndex = mesh * kVertsPerMesh + vid;

    float3 p = unpackA2B10G10R10_SNORM(POSITIONS[vIndex]);

    float c = cos(yaw), s = sin(yaw);
    float3 pr;
    pr.x =  c * p.x + s * p.z;
    pr.y =  p.y;
    pr.z = -s * p.x + c * p.z;
    pr.x += xz.x;
    pr.z += xz.y;

    VSOut o;
    o.pos = mul(g_ViewProj, float4(pr, 1.0));
    o.uv  = in_uv;
    return o;
}

[shader("fragment")]
float4 fs_main(VSOut i) : SV_Target0 {
    return float4(1.0, i.uv.x, i.uv.y, 1.0);
}