# Offline reference for SoD-RT: rebuild one captured frame (SODCAP02) on the CPU and compare
#   (a) the game's stencil shadow volumes (emulated exactly: reversed-Z LESSEQUAL, front +1 / back -1)
#   (b) ray-traced character shadows (shadow test along the light direction against the character meshes)
# on top of the room's baked vertex colours. Writes PNGs.
import sys, struct, math
import numpy as np
from PIL import Image
sys.path.insert(0, 'work/rt_analysis')
import parse_capture2 as pc

cap = sys.argv[1]; out = sys.argv[2] if len(sys.argv) > 2 else 'work/tmp/sodref'
W = int(sys.argv[3]) if len(sys.argv) > 3 else 640
H = int(sys.argv[4]) if len(sys.argv) > 4 else 480

fr, nf, ev = pc.load(cap)
frame = []
for e in ev:
    if e['tag'] == 'PRES': break
    frame.append(e)

def tris_of(d, want_color=False):
    st = d['stride']; vb = d['vb']; nv = len(vb) // st
    if nv == 0: return None, None
    P = np.array([struct.unpack_from('<3f', vb, k * st) for k in range(nv)], dtype=np.float64)
    C = None
    if want_color:
        off = 12 + (12 if d['fvf'] & 0x10 else 0)
        C = np.array([struct.unpack_from('<I', vb, k * st + off)[0] for k in range(nv)], dtype=np.uint32)
    if d['api'] == 1:
        idx = np.array(struct.unpack('<%dH' % (len(d['ib']) // 2), d['ib']), dtype=np.int64) - d['minIndex']
    else:
        idx = np.arange(nv)
    T = []
    if d['primType'] == 5:
        for k in range(len(idx) - 2):
            a, b, c = idx[k], idx[k + 1], idx[k + 2]
            if a == b or b == c or a == c: continue
            T.append((b, a, c) if k & 1 else (a, b, c))
    elif d['primType'] == 4:
        T = [tuple(idx[k:k + 3]) for k in range(0, len(idx) - 2, 3)]
    T = np.array(T, dtype=np.int64)
    if len(T) == 0 or T.max() >= nv or T.min() < 0: return None, None
    return P[T], (C[T] if C is not None else None)       # (n,3,3) positions, (n,3) colours

draws = [e for e in frame if e['tag'] == 'DRAW']
env = [d for d in draws if d['fvf'] == 0x142 and d['api'] == 1 and not d['stencilEnable'] and not d['alphaBlend']]
cam = env[0]
VP = np.array(cam['view']).reshape(4, 4) @ np.array(cam['proj']).reshape(4, 4)
iVP = np.linalg.inv(VP)

def project(P):                    # world (...,3) -> screen x, y, depth, w
    h = np.concatenate([P, np.ones(P.shape[:-1] + (1,))], axis=-1) @ VP
    w = h[..., 3]
    return (h[..., 0] / w + 1) * 0.5 * W, (1 - h[..., 1] / w) * 0.5 * H, h[..., 2] / w, w

def raster(tri_xyzw, cb):          # tri: (3,) x,y,z,w arrays; cb(mask_rows, ys, xs, bary) per covered pixel set
    x, y, z, w = tri_xyzw
    if (w <= 1e-3).any(): return
    x0, x1 = int(max(0, math.floor(x.min()))), int(min(W - 1, math.ceil(x.max())))
    y0, y1 = int(max(0, math.floor(y.min()))), int(min(H - 1, math.ceil(y.max())))
    if x0 > x1 or y0 > y1: return
    area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])
    if abs(area) < 1e-9: return
    ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]; px = xs + 0.5; py = ys + 0.5
    b0 = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area
    b1 = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area
    b2 = 1 - b0 - b1
    m = (b0 >= 0) & (b1 >= 0) & (b2 >= 0)
    if m.any(): cb(ys[m], xs[m], (b0[m], b1[m], b2[m]), area)

# --- 1. room: depth (reversed Z, GREATEREQUAL, clear 0) + baked vertex colour
depth = np.zeros((H, W)); color = np.zeros((H, W, 3))
def argb(c): return np.stack([(c >> 16) & 255, (c >> 8) & 255, c & 255], -1).astype(np.float64)
for d in env:
    T, C = tris_of(d, True)
    if T is None: continue
    sx, sy, sz, sw = project(T)
    for t in range(len(T)):
        col = argb(C[t])
        def cb(ys, xs, b, area, t=t, col=col):
            zz = b[0] * sz[t, 0] + b[1] * sz[t, 1] + b[2] * sz[t, 2]
            ok = zz >= depth[ys, xs]
            ys, xs, zz = ys[ok], xs[ok], zz[ok]
            depth[ys, xs] = zz
            color[ys, xs] = (b[0][ok, None] * col[0] + b[1][ok, None] * col[1] + b[2][ok, None] * col[2])
        raster((sx[t], sy[t], sz[t], sw[t]), cb)
covered = depth > 0
# world position of every pixel from the depth buffer (what the GPU pass will do)
ys, xs = np.mgrid[0:H, 0:W]
ndc = np.stack([(xs + 0.5) / W * 2 - 1, 1 - (ys + 0.5) / H * 2, depth, np.ones_like(depth)], -1)
wp = ndc @ iVP; wp = wp[..., :3] / wp[..., 3:4]

# --- 2. lights (from the volumes) and the game's stencil result per darken quad
groups = []; cur = {'vols': [], 'stencil': np.zeros((H, W), np.int32)}
for e in frame:
    if e['tag'] == 'CLR ' and e['flags'] & 4:
        cur['stencil'][:] = 0
    if e['tag'] != 'DRAW': continue
    d = e
    if d['fvf'] == 0x1d2 and d['stencilEnable']:
        T, _ = tris_of(d)
        if T is None: continue
        sx, sy, sz, sw = project(T)
        inc = 1 if d['stencilPass'] == 7 else -1
        if d['stencilPass'] == 8:
            P = T.reshape(-1, 3);
            vb = d['vb']; st = d['stride']; n = len(vb) // st
            Q = [struct.unpack_from('<3f', vb, k * st) for k in range(n)]
            good = [k for k in range(0, n - 1, 2) if sum((Q[k + 1][j] - Q[k][j]) ** 2 for j in range(3)) > 1]
            dv = np.array(Q[good[0] + 1]) - np.array(Q[good[0]]); dv /= np.linalg.norm(dv)
            near = np.array([Q[k] for k in good])
            cur['vols'].append((dv, near.min(0), near.max(0)))
        for t in range(len(T)):
            def cb(ys, xs, b, area, t=t, cull=d['cull']):
                if (cull == 2 and area > 0) or (cull == 3 and area < 0): return   # D3DCULL_CW / CCW (screen y down: CW = area > 0)
                zz = b[0] * sz[t, 0] + b[1] * sz[t, 1] + b[2] * sz[t, 2]
                ok = zz <= depth[ys, xs]                       # D3DCMP_LESSEQUAL on reversed Z = behind the surface
                np.add.at(cur['stencil'], (ys[ok], xs[ok]), inc)
            raster((sx[t], sy[t], sz[t], sw[t]), cb)
    elif d['fvf'] == 0x44 and d['stencilEnable']:
        a = (struct.unpack_from('<4fI', d['vb'], 0)[4] >> 24) / 255.0
        groups.append({'density': a, 'mask': cur['stencil'] != 0, 'vols': list(cur['vols'])})
        cur = {'vols': [], 'stencil': np.zeros((H, W), np.int32)}

# --- 3. casters: character meshes (FVF 0x1D2, no stencil, opaque) that overlap a shadow volume's caster outline
chars = [d for d in draws if d['fvf'] == 0x1d2 and not d['stencilEnable'] and not d['alphaBlend']]
allvols = [v for g in groups for v in g['vols']]
casters = []
for d in chars:
    T, _ = tris_of(d)
    if T is None: continue
    lo, hi = T.reshape(-1, 3).min(0), T.reshape(-1, 3).max(0)
    if any((lo <= vmax + 30).all() and (hi >= vmin - 30).all() for _, vmin, vmax in allvols):
        casters.append(T)
casterT = np.concatenate(casters) if casters else np.zeros((0, 3, 3))
print('room draws', len(env), 'character draws', len(chars), 'caster draws', len(casters), 'caster tris', len(casterT))

# --- 4. ray-traced mask: ray from each pixel's surface point towards the light (-extrusion) vs caster triangles
def rt_mask(Ltoward):
    m = np.zeros((H, W), bool)
    if len(casterT) == 0: return m
    pts = wp[covered]; idx = np.argwhere(covered)
    # light-space prefilter: only points whose projection lies inside the casters' projected bounds
    u = np.cross(Ltoward, [0, 0, 1.0]); u /= np.linalg.norm(u); v = np.cross(Ltoward, u)
    cu, cv = casterT.reshape(-1, 3) @ u, casterT.reshape(-1, 3) @ v
    pu, pv = pts @ u, pts @ v
    cand = (pu >= cu.min()) & (pu <= cu.max()) & (pv >= cv.min()) & (pv <= cv.max())
    P0 = pts[cand]; hit = np.zeros(len(P0), bool)
    v0, e1, e2 = casterT[:, 0], casterT[:, 1] - casterT[:, 0], casterT[:, 2] - casterT[:, 0]
    pvec = np.cross(Ltoward, e2); det = (e1 * pvec).sum(1)
    ok = np.abs(det) > 1e-9; v0, e1, e2, pvec, det = v0[ok], e1[ok], e2[ok], pvec[ok], det[ok]
    inv = 1 / det
    for s in range(0, len(P0), 2048):                     # Möller–Trumbore, chunked
        o = P0[s:s + 2048, None, :]
        tvec = o - v0[None]
        uu = (tvec * pvec[None]).sum(-1) * inv
        q = np.cross(tvec, e1[None])
        vv = (q @ Ltoward) * inv
        tt = (q * e2[None]).sum(-1) * inv
        hit[s:s + 2048] = ((uu >= 0) & (vv >= 0) & (uu + vv <= 1) & (tt > 2.0)).any(1)
    sel = idx[cand][hit]; m[sel[:, 0], sel[:, 1]] = True
    return m

def compose(masks):
    img = color.copy()
    for g, m in zip(groups, masks):
        img[m] *= (1 - g['density'])                       # darken quad: black, SRCALPHA / INVSRCALPHA
    img[~covered] = 0
    return np.clip(img, 0, 255).astype(np.uint8)

stencil_masks = [g['mask'] & covered for g in groups]
rt_masks = []
for g in groups:
    L = -np.mean([v[0] for v in g['vols']], axis=0); L /= np.linalg.norm(L)
    rt_masks.append(rt_mask(L))
    print('light', np.round(-L, 4), 'density %.3f' % g['density'],
          'stencil px', int(stencil_masks[-1 if False else len(rt_masks) - 1].sum()), 'rt px', int(rt_masks[-1].sum()),
          'agree %.1f%%' % (100 * (stencil_masks[len(rt_masks) - 1] == rt_masks[-1])[covered].mean()))
Image.fromarray(compose([np.zeros((H, W), bool)] * len(groups))).save(out + '_room.png')
Image.fromarray(compose(stencil_masks)).save(out + '_stencil.png')
Image.fromarray(compose(rt_masks)).save(out + '_raytraced.png')
diff = np.zeros((H, W, 3), np.uint8); diff[covered] = 40
for s, r in zip(stencil_masks, rt_masks):
    diff[s & r] = (255, 255, 255); diff[s & ~r] = (255, 60, 60); diff[r & ~s] = (60, 160, 255)
Image.fromarray(diff).save(out + '_diff.png')
np.savez_compressed(out + '_ref.npz', stencil=np.stack(stencil_masks), rt=np.stack(rt_masks), room_depth=depth, color=color)
print('wrote', out + '_*.png')
