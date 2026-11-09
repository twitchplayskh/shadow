// SMAA 1x (Jimenez et al., MIT licence, see LICENSE.txt) adapted to DXVK's built-in pipeline model:
// separate textures (set 1) + the global sampler heap (set 0), sampler indices in push constants.
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 0) uniform sampler s_samplers[];

layout(set = 1, binding = 0) uniform texture2D t_color;    // game image (gamma space)
layout(set = 1, binding = 1) uniform texture2D t_edges;    // pass 1 output
layout(set = 1, binding = 2) uniform texture2D t_blend;    // pass 2 output
layout(set = 1, binding = 3) uniform texture2D t_area;     // AreaTex (D3D10 layout)
layout(set = 1, binding = 4) uniform texture2D t_search;   // SearchTex

layout(push_constant)
uniform smaa_info_t {
  vec4 rt_metrics;          // 1/w, 1/h, w, h
  uint sampler_linear;
  uint sampler_point;
};

#define SMAA_RT_METRICS rt_metrics
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_ULTRA 1

#define SMAATexture2D(tex) texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAA_LIN(tex) sampler2D(tex, s_samplers[sampler_linear])
#define SMAA_PNT(tex) sampler2D(tex, s_samplers[sampler_point])
#define SMAASampleLevelZero(tex, coord) textureLod(SMAA_LIN(tex), coord, 0.0)
#define SMAASampleLevelZeroPoint(tex, coord) textureLod(SMAA_PNT(tex), coord, 0.0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) textureLodOffset(SMAA_LIN(tex), coord, 0.0, offset)
#define SMAASample(tex, coord) texture(SMAA_LIN(tex), coord)
#define SMAASamplePoint(tex, coord) texture(SMAA_PNT(tex), coord)
#define SMAASampleOffset(tex, coord, offset) textureOffset(SMAA_LIN(tex), coord, offset)
#define SMAAGather(tex, coord) textureGather(SMAA_LIN(tex), coord)
#define SMAA_FLATTEN
#define SMAA_BRANCH
#define lerp(a, b, t) mix(a, b, t)
#define saturate(a) clamp(a, 0.0, 1.0)
#define mad(a, b, c) fma(a, b, c)
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4

#include "SMAA.hlsl"

#if SMAA_INCLUDE_PS == 0
vec2 smaa_fullscreen_coord() {
  return vec2(float(gl_VertexIndex & 2), float(gl_VertexIndex & 1) * 2.0f);
}
#endif
