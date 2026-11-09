#version 460
#extension GL_GOOGLE_include_directive : require
#define SMAA_INCLUDE_VS 0
#include "smaa_common.glsl"
layout(location = 0) in vec2 i_texcoord;
layout(location = 1) in vec4 i_offset[3];
layout(location = 0) out vec4 o_edges;
void main() {
  vec4 off[3] = i_offset;
  o_edges = vec4(SMAALumaEdgeDetectionPS(i_texcoord, off, t_color), 0.0f, 0.0f);
}
