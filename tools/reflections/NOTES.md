# SoD room data (ccc.wad) and the SSR material list

- ccc.wad: 665 entries, header [0, 1, usize, stored size incl. header, 0, 0] then zlib blocks [u32 usize <= 0x8000][u32 csize]
  with data 16-aligned (index in ccc.idx: block positions + file headers). evd.wad / mbn.wad / vbi.wad: [usize][csize][zlib].
- Entry: [u32 vertexSection][u32 indexSection][0][0], model blocks from 0x10 (magic 0x19990802 / 0x20000327, chunk list at
  +0x10: [type][size][..] data at +0x10, type 0 ends; next model 16-aligned). Vertex section: FVF 0x142 (xyz, colour, uv),
  24 bytes, exactly what the game uploads. Index section: u16.
- Draw records (in the model chunks): [nv][nidx][minIndex][startIndex][ref][0][u16 1][u16 3]..., strips; the object's
  (vertexBase, indexBase) u32 pair precedes them. ref low 16 bits = runtime texture table index (0xAB3998, 40-byte
  entries, D3D texture at +0x10): positive = a 512x512 page, negative = a sub-texture cut from a page at load; high byte = flags.
- Texture chunk (type 4, magic 0x19990901): page id at +0x20, page size +0x24, count +0x2c, entries {id, x, y, w, h, bpp, q,
  a, sz, 12 bytes} + pixels ((sz - 4) & ~15 bytes, rows 4-aligned), then palettes {id, bpp, q, 8, n, sz, 12} + n bytes BGRA.
  The page is the entries placed at (x, y).
- Cut records {s16 page, s16 slot < 0, x, y, w, h, flags, 0}: coordinates in a 2x page space (x = 2 * px + 1, w = 2 * pw - 3);
  D3D size = next power of two of w / h (copied by 0x7A47F0).
- extract.py: every draw -> triangles -> area / up-facing area per ref and per page sub-image (by UV centroid). Up = +y normal
  by strip winding (carpet at y 0 checks out). 796 floor-like candidates, 565 unique images, labelled in labels.py ->
  reflective_textures.json (M stone/marble/tile, W wood/parquet, L metal).
- Runtime plan: SoDFix exposes the texture table; DXVK (d3d8 side) maps D3D8 texture -> d3d9 texture; per-page R8 masks
  from the sub-image rects, sampled with the draw's own UVs in a replay pass -> per-pixel reflectivity for SSR.
