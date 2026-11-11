# Builds SoDFix\geometry.txt for IMPROVED GEOMETRY: the street paving textures (sheet indices of
# tools/reflections/sheets2.py) with the size of one tile in world units, measured on every up-facing triangle
# that uses them in ccc.wad (texel density = sqrt|det J|, J = d(texel)/d(world x, z); median).
# Needs work/tmp/sod_texuse.pkl and work/tmp/sheet2_items.pkl from the reflection tools.
import sys, os, struct, pickle, numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'reflections'))
from wad import *
STREET = [17, 12, 29, 24, 46, 49, 102, 182, 377]          # grey cobbles / setts of the town streets
r = pickle.load(open('work/tmp/sod_texuse.pkl', 'rb'))
items = {i: keys for i, keys, pos in pickle.load(open('work/tmp/sheet2_items.pkl', 'rb'))}
p2 = lambda v: 1 << (v - 1).bit_length()
targets = {}
for i in STREET:
    for k in items[i]:
        if isinstance(k, tuple):
            _, pid, sid = k; pw, ph, subs = r['pages'][pid][3], r['pages'][pid][4], r['pages'][pid][5]
            for (s, x0, y0, w, h) in subs:
                if s == sid: targets[('p', pid, x0, y0)] = (pw, ph, x0, y0, w, h)
        else:
            cut = r['cuts'].get(k - 0x10000)
            if cut:
                cw, chh = p2(max((cut['w'] + 3) // 2, 1)), p2(max((cut['h'] + 3) // 2, 1))
                targets[('s', k - 0x10000)] = (cw, chh, 0, 0, cw, chh)
ents = set()
for k, u in r['use'].items():
    if isinstance(k, tuple) and k[0] == 'sub' and any(t[0] == 'p' and t[1] == k[1] for t in targets): ents |= u['entries']
    elif isinstance(k, int) and k >= 0x8000 and ('s', k - 0x10000) in targets: ents |= u['entries']
dens = {t: [] for t in targets}
for ei in sorted(ents):
    x = ccc_entry(ei); draws, vs, isec = draw_records(x)
    if not draws: continue
    VV = np.frombuffer(x, np.float32, (isec - vs) // 4, vs).reshape(-1, 6); V = VV[:, :3]; UV = VV[:, 4:6].astype(np.float64)
    for d in draws:
        ref = d['ref'] & 0xFFFF
        cands = [t for t in targets if (t[0] == 'p' and t[1] == ref) or (t[0] == 's' and ref >= 0x8000 and t[1] == ref - 0x10000)]
        if not cands: continue
        idx = np.frombuffer(x, np.uint16, d['ni'], isec + 2 * (d['ib'] + d['si'])).astype(np.int64) + d['vb']
        for j in range(len(idx) - 2):
            tri = [idx[j], idx[j + 1], idx[j + 2]]
            if len(set(tri)) < 3: continue
            P = V[tri]; n = np.cross(P[1] - P[0], P[2] - P[0]); nn = np.linalg.norm(n)
            if nn < 1e-6 or abs(n[1]) / nn < 0.95: continue
            for t in cands:
                pw, ph, x0, y0, w, h = targets[t]
                Tt = UV[tri] * [pw, ph]; cu = Tt.mean(0)
                if t[0] == 'p' and not (x0 <= cu[0] < x0 + w and y0 <= cu[1] < y0 + h): continue
                M = np.array([P[1] - P[0], P[2] - P[0]])[:, [0, 2]]; D = np.array([Tt[1] - Tt[0], Tt[2] - Tt[0]])
                try: J = np.linalg.solve(M, D)
                except np.linalg.LinAlgError: continue
                dd = np.sqrt(abs(np.linalg.det(J)))
                if dd > 1e-4: dens[t].append(dd)
lines = ['# Lebensbaum IMPROVED GEOMETRY: street textures laid out seamlessly in world space',
         '# (generated from ccc.wad by tools/geometry/make_geometry_list.py: texel density measured on every floor triangle)',
         '# page <table index> <page w> <page h> <x> <y> <w> <h> <tile size in world units>',
         '# slot <table index> <tile size in world units>']
for t, v in dens.items():
    if len(v) < 5: continue
    m = float(np.median(v)); pw, ph, x0, y0, w, h = targets[t]
    lines.append('page %d %d %d %d %d %d %d %.1f' % (t[1], pw, ph, x0, y0, w, h, w / m) if t[0] == 'p' else 'slot %d %.1f' % (t[1], w / m))
open(sys.argv[1] if len(sys.argv) > 1 else 'geometry.txt', 'w').write('\n'.join(lines) + '\n')
