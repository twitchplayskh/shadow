#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require
// SoD HDR (dxvk_swapchain_blitter.cpp runSodHdr): the game's standard-range picture -> scRGB (linear BT.709,
// 1.0 = 80 nits), which Windows composes onto the HDR desktop. No other step changes the colours after this: the
// DXVK present blit passes scRGB through (no gamma ramp in windowed mode), and Windows maps scRGB itself.
//
// 1. Reference: the picture as HDR off shows it. The game image is decoded with the chosen curve (HdrGamma) and put at paper white (Windows' SDR content
//    brightness unless HdrPaperWhite says otherwise). Without highlight expansion this IS the HDR-off picture:
//    same colours, gamma, contrast, midtones, shadows; nothing is boosted or tone mapped.
// 2. Highlight expansion, conservative and only for real light: a pixel goes above paper white only if
//    - the game drew it close to white, AND
//    - that brightness came from the game's light draws (additive 3D draws: lamp glows, halos, light effects;
//      collected in the glow image; menus, text and HUD are never in it), AND
//    - by how much light piled up there (overlapping glows that clipped to white in standard range).
//    Bright textures, baked lighting and saturated colours are left alone.
//    The extra brightness rolls off smoothly (1 - e^-x) towards the peak and can never pass it; it keeps the
//    pixel's hue (the colour's channel ratios), easing a little towards white as it brightens instead of turning
//    into more saturated colour. All channels stay >= 0 and <= peak: nothing leaves the BT.709 gamut.
layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2D t_color;   // game image after SMAA (gamma space)
layout(set = 1, binding = 1) uniform texture2D t_glow;    // light draws (gamma-space values, may exceed 1)
layout(set = 1, binding = 2) uniform texture2D t_unused2;
layout(set = 1, binding = 3) uniform texture2D t_unused3;
layout(set = 1, binding = 4) uniform texture2D t_unused4;
layout(push_constant) uniform hdr_info_t {
  vec4 params;            // paper white nits, peak nits, flags (see below), highlight strength
  uint gamma1000;         // decode gamma x 1000 (HdrGamma)
  uint sat1000;           // saturation x 1000 (HdrSaturation; 1000 = the original picture)
};
layout(location = 0) out vec4 o_color;

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

vec3 srgbToLinear(vec3 c) {
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

vec3 decode(vec3 c) { return pow(c, vec3(float(gamma1000) / 1000.0)); }

float max3(vec3 v) { return max(v.r, max(v.g, v.b)); }

void main() {
  ivec2 coord = ivec2(gl_FragCoord.xy);
  float paper = params.x, peak = max(params.y, params.x), strength = params.w;

  // test images (HdrDebug 2..5)
  if (params.z < -4.5) { o_color = vec4(texelFetch(t_unused3, coord % textureSize(t_unused3, 0), 0).rg * 3.0, 0.3, 1.0); return; }
  if (params.z < -3.5) { o_color = vec4(vec2(coord) / vec2(textureSize(t_color, 0)) * 3.0, 0.0, 1.0); return; }
  if (params.z < -2.5) { o_color = vec4(texelFetch(t_color, coord, 0).rgb * 3.0 + vec3(0.0, 0.0, 0.3), 1.0); return; }
  if (params.z < -0.5) { o_color = vec4(1.25, 1.25, 1.25, 1.0); return; }

  bool glowOn = params.z > 0.5;
  const bool leftHalf = false;
  vec3 c = clamp(texelFetch(t_color, coord, 0).rgb, 0.0, 1.0);
  vec3 lin = decode(c);                             // 0..1, 1 = paper white
  lin = max(mix(vec3(dot(lin, kLuma)), lin, float(sat1000) / 1000.0), vec3(0.0));
  vec3 nits = lin * paper;

  if (glowOn && !leftHalf && strength > 0.0 && peak > paper + 1.0) {
    vec3 g = clamp(texelFetch(t_glow, coord, 0).rgb, 0.0, 16.0);
    vec3 glowLin = decode(min(g, vec3(1.0))) + max(g - 1.0, vec3(0.0));
    float Lc = dot(lin, kLuma);
    float Lg = dot(glowLin, kLuma);

    float fromLight = clamp(Lg / max(Lc, 1e-4), 0.0, 1.0);   // share of this pixel's brightness made by light draws
    float nearWhite = smoothstep(0.7, 1.0, max3(c));         // only pixels the game drew close to white
    float piledUp = clamp(Lg - Lc, 0.0, 4.0);                 // light that clipped to white in standard range
    float boost = fromLight * nearWhite * (0.5 + piledUp) * strength;

    float range = peak - paper;
    float extra = range * (1.0 - exp(-boost));               // smooth roll-off towards the peak, never past it
    vec3 hue = lin / max(max3(lin), 1e-4);                    // the pixel's colour, brightest channel at 1
    hue = mix(hue, vec3(1.0), 0.4 * extra / range);           // very bright lights ease towards white
    nits += extra * hue;
  }

  o_color = vec4(nits / 80.0, 1.0);
}
