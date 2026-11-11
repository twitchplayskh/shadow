# Wall textures for IMPROVED GEOMETRY: per wall image (from ssr/wall_items.pkl), measured on every vertical triangle
# that uses it in ccc.wad: texels per world unit along the wall (h) and up (y), the angle of the texture's v axis to
# world down, mirroring, and how much of the image one polygon covers (1 = one whole tile per polygon -> seams).
import sys, pickle, numpy as np, collections
sys.path.insert(0, '/home/claude/sodmap')
from wad import *
r = pickle.load(open('/home/claude/sod/ssr/sod_texuse.pkl', 'rb'))
W = pickle.load(open('/home/claude/sod/ssr/wall_items.pkl', 'rb'))
p2 = lambda v: 1 << (v - 1).bit_length()
targets = {}
for i, keys, area in W:
    for k in keys:
        if isinstance(k, tuple) and len(k) == 5:
            pid, x0, y0, w, h = k
            if pid not in r['pages']: continue
            targets[('p', pid, x0, y0, w, h)] = (r['pages'][pid][3], r['pages'][pid][4], x0, y0, w, h, i)
        elif isinstance(k, tuple) and k[0] == 'slot':
            cut = r['cuts'].get(k[1])
            if cut:
                cw, chh = p2(max((cut['w'] + 3) // 2, 1)), p2(max((cut['h'] + 3) // 2, 1))
                targets[('s', k[1])] = (cw, chh, 0, 0, cw, chh, i)
bypage = collections.defaultdict(list)
for t in targets:
    bypage[('p', t[1]) if t[0] == 'p' else ('s', t[1])].append(t)
ents = set()
for k, u in r['use'].items():
    if isinstance(k, tuple) and k[0] == 'sub' and ('p', k[1]) in bypage: ents |= u['entries']
    elif isinstance(k, int) and k >= 0x8000 and ('s', k - 0x10000) in bypage: ents |= u['entries']
stats = {t: [] for t in targets}
print('entries', len(ents), 'targets', len(targets), file=sys.stderr)
for ei in sorted(ents):
    x = ccc_entry(ei); draws, vs, isec = draw_records(x)
    if not draws: continue
    VV = np.frombuffer(x, np.float32, (isec - vs) // 4, vs).reshape(-1, 6); V = VV[:, :3].astype(np.float64); UV = VV[:, 4:6].astype(np.float64)
    for d in draws:
        ref = d['ref'] & 0xFFFF
        key = ('p', ref) if ref < 0x8000 else ('s', ref - 0x10000)
        cands = bypage.get(key)
        if not cands: continue
        idx = np.frombuffer(x, np.uint16, d['ni'], isec + 2 * (d['ib'] + d['si'])).astype(np.int64) + d['vb']
        for j in range(len(idx) - 2):
            tri = [idx[j], idx[j + 1], idx[j + 2]]
            if len(set(tri)) < 3: continue
            P = V[tri]; n = np.cross(P[1] - P[0], P[2] - P[0]); nn = np.linalg.norm(n)
            if nn < 1e-6 or abs(n[1]) / nn > 0.2: continue                 # vertical-ish only
            nh = np.array([n[0], 0.0, n[2]]); nh /= np.linalg.norm(nh)
            tdir = np.array([-nh[2], 0.0, nh[0]])                            # along the wall
            for t in cands:
                pw, ph, x0, y0, w, h, item = targets[t]
                Tt = UV[tri] * [pw, ph]; cu = Tt.mean(0)
                if t[0] == 'p' and not (x0 - 0.5 <= cu[0] <= x0 + w + 0.5 and y0 - 0.5 <= cu[1] <= y0 + h + 0.5): continue
                Mw = np.array([[np.dot(P[1] - P[0], tdir), P[1][1] - P[0][1]], [np.dot(P[2] - P[0], tdir), P[2][1] - P[0][1]]])
                D = np.array([Tt[1] - Tt[0], Tt[2] - Tt[0]])
                try: J = np.linalg.solve(Mw, D)                               # rows: d(texel)/d(h), d(texel)/d(y)
                except np.linalg.LinAlgError: continue
                su, sv = np.linalg.norm(J[0]), np.linalg.norm(J[1])
                if su < 1e-5 or sv < 1e-5: continue
                ang = np.degrees(np.arctan2(-J[1][0], -J[1][1]))              # texel direction of world "down" vs +v
                span = (np.ptp(Tt[:, 0]) / w, np.ptp(Tt[:, 1]) / h)
                stats[t].append((su, sv, ang, np.sign(np.linalg.det(J)), span[0], span[1], nn / 2, item, J[0][0], J[0][1], J[1][0], J[1][1]))
                break
pickle.dump((targets, stats), open('/tmp/wall_stats.pkl', 'wb'))
