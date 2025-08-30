alias V2 = vec2<f32>;

struct RO_V2 { data: array<V2>; };
struct RW_V2 { data: array<V2>; };
struct I32A  { data: array<i32>; };
struct U32A  { data: array<u32>; };
struct U16A  { data: array<u16>; };

@group(0) @binding(0) var<storage, read>       INSTANCES : RO_V2;
@group(0) @binding(1) var<storage, read_write> VISIBLE   : I32A;
@group(0) @binding(2) var<storage, read_write> COUNTERS  : U32A;  // 5 u32: indexCount, instanceCount, firstIndex, baseVertex, firstInstance
@group(0) @binding(3) var<storage, read>       VERTICES  : RO_V2; // 3 base verts
@group(0) @binding(4) var<storage, read_write> VARYINGS  : RW_V2; // vertex buffer for VS
@group(0) @binding(5) var<storage, read_write> INDICES   : U16A;  // index buffer (u16)
@group(0) @binding(6) var<storage, read_write> DISPATCH  : U32A;  // 3 u32 for DispatchWorkgroupsIndirect

@compute @workgroup_size(1)
fn cs_instance() {
  let inst_count: u32 = 4u;   // you upload 4 instances
  let verts_per : u32 = 3u;
  let total     : u32 = inst_count * verts_per;

  for (var i:u32=0u; i<inst_count && i<u32(arrayLength(&VISIBLE.data)); i++) {
    VISIBLE.data[i] = 1;
  }

  for (var i:u32=0u; i<inst_count; i++) {
    let off: V2 = INSTANCES.data[i];
    let base: u32 = i * verts_per;

    VARYINGS.data[base + 0u] = VERTICES.data[0u] + off;
    VARYINGS.data[base + 1u] = VERTICES.data[1u] + off;
    VARYINGS.data[base + 2u] = VERTICES.data[2u] + off;

    INDICES.data[base + 0u] = u16(base + 0u);
    INDICES.data[base + 1u] = u16(base + 1u);
    INDICES.data[base + 2u] = u16(base + 2u);
  }

  if (arrayLength(&COUNTERS.data) >= 5u) {
    COUNTERS.data[0] = total;
    COUNTERS.data[1] = 1u;
    COUNTERS.data[2] = 0u;
    COUNTERS.data[3] = 0u;
    COUNTERS.data[4] = 0u;
  }

  if (arrayLength(&DISPATCH.data) >= 3u) {
    DISPATCH.data[0] = 1u; DISPATCH.data[1] = 1u; DISPATCH.data[2] = 1u;
  }
}

@compute @workgroup_size(1) fn cs_prepare() {}
@compute @workgroup_size(1) fn cs_meshlet() {}

struct VSOut { @builtin(position) pos: vec4<f32>; @location(0) c: vec3<f32>; };

@vertex
fn vs_main(@location(0) in_pos: V2) -> VSOut {
  var o: VSOut;
  o.pos = vec4<f32>(in_pos, 0.0, 1.0);
  o.c = 0.5 + 0.5 * vec3<f32>(in_pos.x, in_pos.y, 1.0 - in_pos.x * in_pos.y);
  return o;
}

@fragment
fn fs_main(inp: VSOut) -> @location(0) vec4<f32> { return vec4<f32>(inp.c, 1.0); }
