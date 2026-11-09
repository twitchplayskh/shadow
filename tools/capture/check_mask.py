# Check a SoD-RT shadow mask (sod_rt_mask_N.bin / .sdmk): independent CPU ray trace from the same depth + geometry,
# per-light bit comparison, and (optional) comparison with the game's own stencil shadows from offline_ref.py.
# usage: check_mask.py file.sdmk [ref.npz] [out_prefix]
import sys, struct
import numpy as np
from PIL import Image

path = sys.argv[1]
refp = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2].endswith('.npz') else None
outp = sys.argv[3] if len(sys.argv) > 3 else 'work/tmp/maskcheck'
d = open(path, 'rb').read()
magic, ver, frame, W, H, gb = struct.unpack_from('<6I', d, 0)
assert magic == 0x4B4D4453, 'not an SDMK file'
o = 24; g = d[o:o + gb]; o += gb
depth = np.frombuffer(d, np.float32, W * H, o).reshape(H, W); o += W * H * 4
mask = np.frombuffer(d, np.uint8, W * H, o).reshape(H, W)
gm, gframe, gw, gh = struct.unpack_from('<4I', g, 0)
vpX, vpY, vpW, vpH = struct.unpack_from('<4f', g, 16)
iVP = np.array(struct.unpack_from('<16f', g, 32), np.float64).reshape(4, 4)
nl, nc, nt, flags = struct.unpack_from('<4I', g, 96)
lights = [struct.unpack_from('<4f', g, 112 + 16 * i) for i in range(nl)]
casters = [struct.unpack_from('<4I', g, 240 + 16 * i) for i in range(nc)]
tris = np.frombuffer(g, np.float32, nt * 9, gb - nt * 36).reshape(nt, 3, 3).astype(np.float64)   # header size varies by version
print('frame %u %ux%u lights %u casters %u tris %u' % (frame, W, H, nl, nc, nt))
for i, (x, y, z, a) in enumerate(lights): print('  light %d toLight (%.4f %.4f %.4f) density %.3f' % (i, x, y, z, a))
for i, (f0, n, m, _) in enumerate(casters): print('  caster %d tris %u..%u lightMask %x' % (i, f0, f0 + n, m))

# world positions exactly like rtmask.comp
ys, xs = np.mgrid[0:H, 0:W]
valid = (depth > 0) & (depth <= 1)
hpos = np.stack([((xs + 0.5) - vpX) / vpW * 2 - 1, 1 - ((ys + 0.5) - vpY) / vpH * 2, depth.astype(np.float64), np.ones((H, W))], -1)
wp = hpos @ iVP; wp = wp[..., :3] / wp[..., 3:4]

cpu = np.zeros((H, W), np.uint8)
tri_caster = np.zeros(nt, np.int64)
for ci, (f0, n, m, _) in enumerate(casters): tri_caster[f0:f0 + n] = ci
for l, (lx, ly, lz, a) in enumerate(lights):
    L = np.array([lx, ly, lz])
    sel = np.array([(casters[c][2] >> l) & 1 for c in tri_caster], bool)
    T = tris[sel]
    if not len(T): continue
    u = np.cross(L, [0, 0, 1.0]); u /= np.linalg.norm(u); v = np.cross(L, u)
    cu, cv = T.reshape(-1, 3) @ u, T.reshape(-1, 3) @ v
    pts = wp[valid]; idx = np.argwhere(valid)
    pu, pv = pts @ u, pts @ v
    cand = (pu >= cu.min() - 1) & (pu <= cu.max() + 1) & (pv >= cv.min() - 1) & (pv <= cv.max() + 1)
    P0 = pts[cand] + L * 1.0; hit = np.zeros(len(P0), bool)
    v0, e1, e2 = T[:, 0], T[:, 1] - T[:, 0], T[:, 2] - T[:, 0]
    pvec = np.cross(L, e2); det = (e1 * pvec).sum(1); ok = np.abs(det) > 1e-12
    v0, e1, e2, pvec, inv = v0[ok], e1[ok], e2[ok], pvec[ok], 1 / det[ok]
    for s in range(0, len(P0), 1024):
        tv = P0[s:s + 1024, None, :] - v0[None]
        uu = (tv * pvec[None]).sum(-1) * inv
        q = np.cross(tv, e1[None])
        vv = (q @ L) * inv; tt = (q * e2[None]).sum(-1) * inv
        hit[s:s + 1024] = ((uu >= 0) & (vv >= 0) & (uu + vv <= 1) & (tt >= 0.5) & (tt <= 100000)).any(1)
    sel_px = idx[cand][hit]
    cpu[sel_px[:, 0], sel_px[:, 1]] |= 1 << l

# result encoding: flags bit 0 = light factor (255 = lit, else 255 * prod(1 - density)), otherwise bit per light
factor_mode = bool(flags & 1)
if factor_mode:
    lit = np.ones((H, W))
    for l in range(nl): lit = np.where((cpu >> l) & 1, lit * (1 - lights[l][3]), lit)
    expect = np.where(valid, np.clip(np.floor(lit * 255 + 0.5), 0, 255), 255).astype(np.int32)
    diff = np.abs(mask.astype(np.int32) - expect)
    nshadow = int((expect != 255).sum())
    print('light factor: kernel shadowed %d px, CPU %d px, differing %d px (%.3f%% of shadow), max byte diff %d' %
          ((mask != 255).sum(), nshadow, int((diff > 1).sum()), 100 * (diff > 1).sum() / max(1, nshadow), diff.max()))
    ok = (diff > 1).sum() / max(1, nshadow) < 0.01
    # per-light bits for the comparisons below: from the factor (single-light frames are exact)
    gpu_bits = np.zeros((H, W), np.uint8)
    for l in range(nl):
        gpu_bits |= (((cpu >> l) & 1) & (mask != 255)).astype(np.uint8) << l
    mask = gpu_bits
else:
    ok = True
    for l in range(nl):
        g_ = (mask >> l) & 1; c_ = (cpu >> l) & 1
        diff = int((g_ != c_).sum())
        edge = diff / max(1, int(c_.sum()))
        print('light %d: kernel %d px, CPU %d px, differing %d (%.3f%% of shadow)' % (l, g_.sum(), c_.sum(), diff, 100 * edge))
        ok &= edge < 0.01
print('kernel vs CPU ray trace:', 'MATCH' if ok else 'DIFFERENT')

if refp:
    r = np.load(refp)
    st, room = r['stencil'], r['room_depth']
    if st.shape[1:] == (H, W):
        roomonly = valid & (np.abs(room - depth) < 2e-4) & (room > 0)   # 24-bit depth vs float raster       # pixels not covered by a character
        for l in range(min(nl, len(st))):
            g_ = ((mask >> l) & 1).astype(bool) & roomonly; s_ = st[l] & roomonly
            inter = (g_ & s_).sum(); union = (g_ | s_).sum()
            print('light %d vs game stencil (room pixels): RT %d, stencil %d, overlap %.1f%% (IoU)' % (l, g_.sum(), s_.sum(), 100 * inter / max(1, union)))
        img = r['color'].copy()
        for l in range(nl):
            m_ = ((mask >> l) & 1).astype(bool) & roomonly
            img[m_] *= (1 - lights[l][3])
        Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(outp + '_kernel_on_room.png')
        diffimg = np.full((H, W, 3), 40, np.uint8)
        for l in range(min(nl, len(st))):
            g_ = ((mask >> l) & 1).astype(bool) & roomonly; s_ = st[l] & roomonly
            diffimg[g_ & s_] = (255, 255, 255); diffimg[s_ & ~g_] = (255, 60, 60); diffimg[g_ & ~s_] = (60, 160, 255)
        Image.fromarray(diffimg).save(outp + '_kernel_vs_stencil.png')
vis = np.zeros((H, W, 3), np.uint8); vis[valid] = 60
vis[(mask & 1) != 0] = (255, 255, 255); vis[(mask & 2) != 0] = (255, 160, 60)
Image.fromarray(vis).save(outp + '_mask.png')
print('images:', outp + '_*.png')
