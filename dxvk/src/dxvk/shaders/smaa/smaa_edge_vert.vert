#version 460
#extension GL_GOOGLE_include_directive : require
#define SMAA_INCLUDE_PS 0
#include "smaa_common.glsl"
layout(location = 0) out vec2 o_texcoord;
layout(location = 1) out vec4 o_offset[3];
void main() {
  vec2 c = smaa_fullscreen_coord();
  o_texcoord = c;
  gl_Position = vec4(-1.0f + 2.0f * c, 0.0f, 1.0f);
  vec4 off[3];
  SMAAEdgeDetectionVS(c, off);
  o_offset = off;
}
