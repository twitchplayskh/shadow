# Surface depth (parallax relief on ground textures)

The rooms are drawn with the fixed-function pipeline (FVF 0x142: position, colour, one UV set), so there is no
game shader to extend. The effect runs in screen space, after the opaque scene, in the same place as the
reflections.

## Which surfaces

`SoDFix\relief.txt` is built from `ccc.wad` by `tools/relief/make_relief_list.py`: the up-facing texture usage of
every room (the reflection tools' `sod_texuse.pkl`), labelled by eye into cobblestones (C), paving slabs and
flagstones (P) and brick paving (B). Same line format as `reflect.txt`: `page` regions of a texture page, or `slot`
sub-textures. Texture identity at run time goes through the game's texture table (0xAB3998), as for reflections.

## Height and normal maps (`sod_relief_gen.comp`)

The first time a listed texture is drawn, its top mip level is copied on the GPU into a raw buffer and eight short
compute passes build the maps into atlas buffers (8 MB albedo, 32 MB relief, kept for the session). The relief
is built at up to 4x the texture's resolution (most ground textures are 32x32 or 64x64) from a bilinear
reconstruction, so joints become clean lines:

1. decode (A8R8G8B8, A1R5G5B5, R5G6B5, A4R4G4B4 and their Vulkan variants), luminance
2. light smoothing
3. joint score: largest Hessian eigenvalue (dark line) at two scales, darkness against the local mean in local
   standard deviations, colour difference from the neighbourhood; histogram
4. joints = the material's share of the surface (30 % cobbles, 14 % slabs, 16 % bricks) from the histogram
5. keep joint texels with support along a line (drops stains and specks: dark is not the same as deep)
6. distance from every texel to the nearest joint
7. height: flat stone tops with edges rounded over a material width (quarter-circle profile), a gentle crown on
   cobbles, a little grain
8. normalise, slopes for the normal map

Everything wraps (floor textures tile). Up to four textures are generated per frame.

## Per frame (`sod_relief.comp`)

The recorded ground draws are drawn again into an R16G16B16A16 target, depth-tested against the game's depth:
R, G = the draw's own UVs through a 256x256 identity texture sampled bilinear with MIRROR addressing (no seam at
the tile wrap, 1/65536 precision), B = which half of the mirror period (a 2x2 texture at uv / 2), A = entry id.
The compute pass rebuilds the texel frame per pixel from neighbouring depth and UVs, marches the height field
(12 / 24 steps, secant refinement), and writes `frame colour x albedo(shifted) / albedo(original)` so the game's
vertex lighting, fog and baked shadows stay, plus normal-map shading from a fixed sky direction, self-shadowing
(HIGH) and a little cavity darkening. It fades out where the texture is minified (no mips in the atlas), in fog
and at grazing angles, and on surfaces that don't face up (walls sharing a listed texture). Near any place where
the texture mapping ends (a UV seam between polygons, another material, a surface without relief) the depth is
feathered to zero over about three texels, so edges blend in. Derivatives are only taken from neighbours whose
mapping continues smoothly two pixels out. The result is blended over the frame before reflections and ambient occlusion.

Tuning: `SODRT_RELIEF=0/1/2` overrides the menu, `SODRT_RELIEF_DEPTH` scales the depth, `SODRT_RELIEF_DEBUG=1`
shows the height, `2` the material ids.
