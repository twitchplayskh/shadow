#version 460
#extension GL_GOOGLE_include_directive : require
#define SMAA_INCLUDE_VS 0
#include "smaa_common.glsl"
layout(location = 0) in vec2 i_texcoord;
layout(location = 1) in vec4 i_offset;
layout(location = 0) out vec4 o_color;
void main() {
  o_color = SMAANeighborhoodBlendingPS(i_texcoord, i_offset, t_color, t_blend);
}
