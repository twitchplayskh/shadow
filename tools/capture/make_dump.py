# Build an SDMK verification file (same layout d3d9.dll writes) from a SODCAP02 capture, so the helper's
# ray-query kernel can be run offline (lavapipe) on exactly the data the game side would send:
#   - lights: one per darken quad, from the volumes since the last stencil clear (DECR pass only)
#   - casters: FVF 0x1D2 non-stencil draws, de-duplicated by (vb, startIndex, primCount), grouped by vertex buffer,
#              light mask from volume/caster bounds overlap (30 units), like BridgeLink::BuildGeometry
#   - camera: inverse(view * proj), row vectors
#   - depth: rasterized room + characters (reversed Z), as the depth buffer holds at the trigger point
# usage: make_dump.py capture.bin out.sdmk [width height]
import sys, struct, math, os
import numpy as np
sys.path.insert(0, 'work/rt_analysis')
import parse_capture2 as pc

cap, out = sys.argv[1], sys.argv[2]
W = int(sys.argv[3]) if len(sys.argv) > 3 else 640
H = int(sys.argv[4]) if len(sys.argv) > 4 else 480
fr, nf, ev = pc.load(cap)
frame = []
for e in ev:
    if e['tag'] == 'PRES': break
    frame.append(e)

def positions(d):
    st = d['stride']; vb = d['vb']; n = len(vb) // st
    return np.array([struct.unpack_from('<3f', vb, k * st) for k in range(n)], dtype=np.float64)

def tris_of(d):
    P = positions(d)
    idx = np.array(struct.unpack('<%dH' % (len(d['ib']) // 2), d['ib']), dtype=np.int64) - d['minIndex']
    T = []
    for k in range(len(idx) - 2):
        a, b, c = idx[k], idx[k + 1], idx[k + 2]
        if a == b or b == c or a == c or max(a, b, c) >= len(P) or min(a, b, c) < 0: continue
        T.append((b, a, c) if k & 1 else (a, b, c))
    return P[np.array(T, dtype=np.int64)] if T else np.zeros((0, 3, 3))

# ---- walk the frame like SodLinkObserveDraw
cur_vols, lights, casters, keys = [], [], {}, set()
camera = None; trigger_at = None
for i, e in enumerate(frame):
    if e['tag'] == 'CLR ' and e['flags'] & 4:
        cur_vols = []
        continue
    if e['tag'] != 'DRAW': continue
    d = e
    if d['fvf'] == 0x1d2 and d['api'] == 1:
        camera = (d['view'], d['proj'])
        if d['stencilEnable']:
            if d['stencilPass'] != 8: continue
            P = positions(d)
            dirv = None; lo = np.full(3, 1e30); hi = np.full(3, -1e30)
            A = np.zeros((3, 3)); bvec = np.zeros(3); minDot = 1.0
            for k in range(0, len(P) - 1, 2):
                dv = P[k + 1] - P[k]; L = np.linalg.norm(dv)
                if L < 1.0: continue
                if dirv is None: dirv = dv / L
                lo = np.minimum(lo, P[k]); hi = np.maximum(hi, P[k])
                u = dv / L; minDot = min(minDot, float(u @ dirv)); M = np.eye(3) - np.outer(u, u); A += M; bvec += M @ P[k]
            if dirv is not None: cur_vols.append((dirv, lo, hi, A, bvec, minDot))
            continue
        if d['alphaBlend']: continue          # alpha-blended parts/overlays: the game builds no volume for them
        key = (d['vbPtr'], d['startIndex'], d['primCount'])
        if key in keys: continue
        keys.add(key)
        T = tris_of(d)
        casters.setdefault(d['vbPtr'], []).append(T)
        continue
    if d['api'] == 2 and d['stencilEnable'] and d['fvf'] == 0x44:
        a = (struct.unpack_from('<4fI', d['vb'], 0)[4] >> 24) / 255.0
        if cur_vols: lights.append((a, cur_vols))
        cur_vols = []
        continue
    if lights and trigger_at is None:
        trigger_at = i
        break

print('lights', len(lights), 'casters', len(casters), 'trigger at event', trigger_at)
view = np.array(camera[0]).reshape(4, 4); proj = np.array(camera[1]).reshape(4, 4)
VP = view @ proj; iVP = np.linalg.inv(VP)

# ---- geometry frame (struct SodGeomFrame)
glights = []; gpoint = []
for a, vols in lights:
    dsum = np.sum([v[0] for v in vols], axis=0); dsum /= np.linalg.norm(dsum)
    minDot = min([v[5] for v in vols] + [float(v[0] @ dsum) for v in vols])
    if minDot < 0.9999 and not os.environ.get('NOPOINT'):
        X = np.linalg.solve(sum(v[3] for v in vols), sum(v[4] for v in vols))
        glights.append((X, a)); gpoint.append(True); print('point light at', np.round(X, 1), 'density', a)
    else:
        glights.append((-dsum, a)); gpoint.append(False); print('parallel light toward', np.round(-dsum, 3), 'density', a)
gcasters, tris = [], []
ntri = 0
for vb, parts in casters.items():
    T = np.concatenate(parts)
    lo, hi = T.reshape(-1, 3).min(0), T.reshape(-1, 3).max(0)
    mask = 0
    for l, (a, vols) in enumerate(lights):
        if any((lo <= v[2] + 30).all() and (hi >= v[1] - 30).all() for v in vols): mask |= 1 << l
    import os
    if os.environ.get('FORCEMASK'): mask = int(os.environ['FORCEMASK'], 0)
    if not mask: continue
    gcasters.append((ntri, len(T), mask)); tris.append(T.astype(np.float32)); ntri += len(T)
print('caster masks', [hex(c[2]) for c in gcasters], 'tris', ntri)
hdr = struct.pack('<4I4f16f4I', 0x4D4F4547, fr, W, H, 0.0, 0.0, float(W), float(H), *iVP.astype(np.float32).reshape(-1),
                  len(glights), len(gcasters), ntri, 1)   # flags bit 0: light-factor output
hdr += b''.join(struct.pack('<4f', *L, a) for L, a in glights) + b'\0' * 16 * (8 - len(glights))
hdr += b''.join(struct.pack('<4I', f, n, m, 0) for f, n, m in gcasters) + b'\0' * 16 * (64 - len(gcasters))
assert len(hdr) == 1264
# per light: casters' bounds along the light (same as BridgeLink::BuildGeometry)
allT = np.concatenate(tris).astype(np.float64) if tris else np.zeros((0, 3, 3))
boxes = b''
for l in range(8):
    if l >= len(glights): boxes += b'\0' * 48; continue
    if gpoint[l]:
        sel = [allT[f0:f0 + n].reshape(-1, 3) for f0, n, m in gcasters if (m >> l) & 1]
        if not sel: boxes += struct.pack('<12f', 0, 0, 0, -1, *([0.0] * 7), 1.0); continue
        P = np.concatenate(sel); lo, hi = P.min(0), P.max(0); c = (lo + hi) / 2; r = np.linalg.norm((hi - lo) / 2)
        boxes += struct.pack('<12f', *c, r, float(os.environ.get('LAMP', '30')), *([0.0] * 6), 1.0); continue
    L = np.array(glights[l][0], dtype=np.float32).astype(np.float64)
    a_ = np.array([0.0, 1.0, 0.0]) if abs(L[0]) > 0.9 else np.array([1.0, 0.0, 0.0])
    u = np.cross(L, a_); u /= np.linalg.norm(u); v = np.cross(L, u)
    pts = np.concatenate([allT[f0:f0 + n].reshape(-1, 3) for f0, n, m in gcasters if (m >> l) & 1]) if any((m >> l) & 1 for _, _, m in gcasters) else None
    if pts is None:
        boxes += struct.pack('<12f', *u, 1e30, *v, -1e30, 1e30, -1e30, -1e30, 0); continue
    pu, pv, pl = pts @ u, pts @ v, pts @ L
    boxes += struct.pack('<12f', *u, pu.min(), *v, pu.max(), pv.min(), pv.max(), pl.max(), 0)
hdr += boxes
assert len(hdr) == 1264 + 384
geom = hdr + b''.join(t.tobytes() for t in tris)

# ---- depth at the trigger point: everything drawn before it (room + characters), reversed Z, GREATEREQUAL
depth = np.zeros((H, W))
def project(P):
    h = np.concatenate([P, np.ones(P.shape[:-1] + (1,))], -1) @ VP
    return (h[..., 0] / h[..., 3] + 1) * 0.5 * W, (1 - h[..., 1] / h[..., 3]) * 0.5 * H, h[..., 2] / h[..., 3], h[..., 3]
for e in frame[:trigger_at]:
    if e['tag'] != 'DRAW' or e['stencilEnable'] or not e['zWrite'] or e['fvf'] not in (0x142, 0x1d2) or e['api'] != 1: continue
    T = tris_of(e)
    if not len(T): continue
    sx, sy, sz, sw = project(T)
    for t in range(len(T)):
        x, y, z, w = sx[t], sy[t], sz[t], sw[t]
        if (w <= 1e-3).any(): continue
        x0, x1 = int(max(0, math.floor(x.min()))), int(min(W - 1, math.ceil(x.max())))
        y0, y1 = int(max(0, math.floor(y.min()))), int(min(H - 1, math.ceil(y.max())))
        if x0 > x1 or y0 > y1: continue
        area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])
        if abs(area) < 1e-9: continue
        ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]; px = xs + 0.5; py = ys + 0.5
        b0 = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area
        b1 = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area
        b2 = 1 - b0 - b1
        m = (b0 >= 0) & (b1 >= 0) & (b2 >= 0)
        zz = b0 * z[0] + b1 * z[1] + b2 * z[2]
        m &= zz >= depth[ys, xs]
        depth[ys[m], xs[m]] = zz[m]
with open(out, 'wb') as f:
    f.write(struct.pack('<6I', 0x4B4D4453, 1, fr, W, H, len(geom)))
    f.write(geom)
    f.write(depth.astype(np.float32).tobytes())
    f.write(np.full(W * H, 255, np.uint8).tobytes())
print('wrote', out, 'depth coverage %.1f%%' % (100 * (depth > 0).mean()))
