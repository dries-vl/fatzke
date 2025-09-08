// WINDOWS: scoop install main/vulkan (for slangc)
// WINDOWS: scoop install main/ktx-software (for toktx/ktx)
// WINDOWS: scoop install main/vim (for xxd)
// ARCH: yay -S ktx-software-bin (for toktx/ktx)
// source ~/Downloads/1.4.321.1/setup-env.sh (vulkan sdk on linux, for slangc)
/* slangc shaders.slang -entry VS_Tri -entry PS_Tri -entry VS_Blit
-entry PS_Blit -target spirv -fvk-use-entrypoint-name -o static/shaders.spv; */
// xxd -i static/shaders.spv > static/shaders.h; rm static/shaders.spv
struct VOutA {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

[shader("vertex")]
VOutA VS_Tri(uint vid: SV_VertexID) {
  static const float2 P[3] = { float2(-1.0, -1.0), float2(0.0, 1.0),
                               float2(1.0, -1.0) };
  static const float2 UV[3] = { float2(0.0, 0.0), float2(0.5, 1.0),
                               float2(1.0, 0.0) };
  float2 p = P[vid];
  VOutA o;
  o.pos = float4(p, 0, 1);
  o.uv = UV[vid];
  return o;
}

[[vk::binding(0, 0)]]
Texture2D uTex;
[[vk::binding(1, 0)]]
SamplerState uSamp;

[shader("pixel")]
float4 PS_Tri(VOutA i) : SV_Target { return uTex.Sample(uSamp, i.uv); }

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

[shader("pixel")]
float4 PS_Blit(VSOutBlit i) : SV_Target { return uTex.Sample(uSamp, i.uv); }
