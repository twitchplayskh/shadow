#version 460
// SoD HDR expansion: fullscreen triangle
void main() {
  vec2 c = vec2(float(gl_VertexIndex & 2), float(gl_VertexIndex & 1) * 2.0f);
  gl_Position = vec4(-1.0f + 2.0f * c, 0.0f, 1.0f);
}
