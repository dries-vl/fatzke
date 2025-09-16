struct Instance { offset: vec2<f32> };
struct Counters { index_count: atomic<u32>, instance_count: atomic<u32>, first_index: atomic<u32>, base_vertex: atomic<u32>, first_instance: atomic<u32> };

@group(0) @binding(0) var<storage,read> INSTANCES: array<Instance>; // IN
@group(0) @binding(1) var<storage,read_write> VISIBLE: array<u32>; // OUT
@group(0) @binding(2) var<storage,read_write> COUNTERS: Counters; // OUT
@group(0) @binding(6) var<storage,read_write> DISPATCH : array<u32,3>; // OUT
@group(0) @binding(3) var<storage,read> VERTICES: array<vec2<f32>>; // IN  ~hardcoded 3 vertices data in here
@group(0) @binding(4) var<storage,read_write> VARYINGS: array<vec2<f32>>; // OUT  ~vertex buffer written by meshlet shader
@group(0) @binding(5) var<storage,read_write> INDICES: array<u32>; // OUT  ~index buffer written by meshlet shader

// ---------- compute 1 : instance → visible ----------
@compute @workgroup_size(64) // todo: we have 64 here, but not really using the same memory at all per instance; still use 64?
fn cs_instance(@builtin(global_invocation_id) id: vec3<u32>) {
    if (id.x >= arrayLength(&INSTANCES)) { return; }
    let slot = atomicAdd(&COUNTERS.instance_count, 1u); // todo: atomic makes no sense without culling/lod, but we'll add it later
    VISIBLE[slot] = id.x;
}

// ---------- compute 2 : visible to workgroup size ----------
@compute @workgroup_size(1)
fn cs_prepare() {
    /* x  = number of visible meshlets  */
    DISPATCH[0] = atomicLoad(&COUNTERS.instance_count);
    DISPATCH[1] = 1u;        /* y  */
    DISPATCH[2] = 1u;        /* z  */
}

// ---------- compute 3 : visible instance → 1 triangle ----------
var<workgroup> vertices: array<vec2<f32>, 64>;
var<workgroup> instance: Instance;
var<workgroup> instance_id: u32;

@compute @workgroup_size(64)
fn cs_meshlet(@builtin(workgroup_id) wg: vec3<u32>, @builtin(local_invocation_id) lid: vec3<u32>) {
    // todo: we shouldn't need do this, instead only dispatch the amount of meshlets that the instance compute pass writes
    if (wg.x >= atomicLoad(&COUNTERS.instance_count)) { return; }

    // load data to LDS
    vertices[lid.x] = VERTICES[lid.x]; // todo: offset into the buffer to get this meshlet's data
    if (lid.x == 0u) {
        instance_id = VISIBLE[wg.x];
        instance = INSTANCES[instance_id];
    }
    workgroupBarrier(); // wait for all data to be loaded into LDS

    // write three vertices
    if (lid.x >= 3u) {return;} // todo: replace this with comparing the invocation id with the nr of verts in the meshlet
    let base = instance_id * 3u;
    VARYINGS[base + lid.x] = vertices[lid.x] + instance.offset; // write the vertex data (just the NDC of the vertices now) + the offset of the instance

    // write the triangle
    if (lid.x == 0u) {
        let i_base = atomicAdd(&COUNTERS.index_count, 3u); // todo: can we avoid this atomic somehow?
        INDICES[i_base+0u] = i_base + 0u;
        INDICES[i_base+1u] = i_base + 1u;
        INDICES[i_base+2u] = i_base + 2u;
    }
}

// ---------- raster shaders ----------
struct VSIn  { @location(0) pos : vec2<f32> };
struct VSOut { @builtin(position) pos : vec4<f32> };

@vertex
fn vs_main(i:VSIn) -> VSOut { return VSOut(vec4<f32>(i.pos,0.,1.)); }

@fragment
fn fs_main() -> @location(0) vec4<f32> { return vec4<f32>(100.,0.4,0.1,1); }
