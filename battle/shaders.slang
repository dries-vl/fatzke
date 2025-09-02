// source ~/Downloads/1.4.321.1/setup-env.sh
// slangc shaders.slang -entry VS_Tri -entry PS_Tri -entry VS_Blit -entry
// PS_Blit -target spirv -fvk-use-entrypoint-name -o shaders.spv
struct Vertex {
  float4 pos : SV_Position;
  float3 col : COLOR0;
};
[shader("vertex")]
Vertex VS_Tri(uint vid: SV_VertexID) {
  static const float2 P[3] = { float2(-0.5, -0.5), float2(0.0, 0.5), float2(0.5, -0.5) };
  float2 p = P[vid];
  Vertex o;
  o.pos = float4(p, 0, 1);
  o.col = float3(p * 0.5 + 0.5, 1);
  return o;
}
[shader("pixel")]
float4 PS_Tri(Vertex i) : SV_Target { return float4(i.col, 1); }

struct VSOutBlit {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};
[shader("vertex")]
VSOutBlit VS_Blit(uint vid: SV_VertexID) {
  static const float2 P[3] = { float2(-1, -1), float2(3, -1), float2(-1, 3) };
  float2 pos = P[vid];
  VSOutBlit o;
  o.pos = float4(pos, 0, 1);
  o.uv = pos * 0.5 + 0.5;
  return o;
}
// Bindings: set=0,binding=0/1 (adjust if you prefer different layout)
[[vk::binding(0, 0)]]
Texture2D uTex;
[[vk::binding(0, 1)]]
SamplerState uSamp;
[shader("pixel")]
float4 PS_Blit(VSOutBlit i) : SV_Target { return uTex.Sample(uSamp, i.uv); }
