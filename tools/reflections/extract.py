import os
# Per texture reference over all ccc.wad rooms: total area, area facing up / down, entries using it.
import sys, struct, pickle, numpy as np
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from wad import *
res = {'pages': {}, 'cuts': {}, 'use': {}, 'errors': []}
for ei in range(nentries()):
    try:
        x = ccc_entry(ei)
        pids = []
        for c, sz in tex_chunks(x):
            pid = struct.unpack_from('<I', x, c + 0x20)[0]
            if pid not in res['pages']:
                pw, ph = struct.unpack_from('<HH', x, c + 0x10 + 0x14)
                ents, _ = tex_entries(x, c, sz)
                res['pages'][pid] = (ei, c, sz, pw, ph, [(v['id'], v['x'], v['y'], v['w'], v['h']) for v in ents])
            pids.append(pid)
        draws, vs, isec = draw_records(x)
        if not draws: continue
        refpages = set()
        for d in draws:
            lo = d['ref'] & 0xFFFF
            if lo < 0x8000: refpages.add(lo)
        for k in cut_records(x, sorted(refpages | set(pids))):
            res['cuts'].setdefault(k['slot'], dict(k, entry=ei))
        VV = np.frombuffer(x, np.float32, (isec - vs) // 4, vs).reshape(-1, 6); V = VV[:, :3]; UV = VV[:, 4:6]
        for d in draws:
            idx = np.frombuffer(x, np.uint16, d['ni'], isec + 2 * (d['ib'] + d['si'])).astype(np.int64) + d['vb']
            if len(idx) < 3: continue
            a, b, c3 = idx[:-2], idx[1:-1], idx[2:]
            flip = (np.arange(len(a)) & 1).astype(bool)
            b2 = np.where(flip, c3, b); c2 = np.where(flip, b, c3)
            ok = (a != b) & (b != c3) & (a != c3)
            if not ok.any(): continue
            p0, p1, p2 = V[a[ok]], V[b2[ok]], V[c2[ok]]
            n = np.cross(p1 - p0, p2 - p0); ar = np.linalg.norm(n, axis=1) * 0.5
            ny = n[:, 1] / np.maximum(2 * ar, 1e-9)
            key = d['ref'] & 0xFFFF
            u = res['use'].setdefault(key, dict(area=0.0, pos=0.0, neg=0.0, draws=0, entries=set(), flags=set(), ymin=1e9, ymax=-1e9))
            u['area'] += float(ar.sum()); u['pos'] += float(ar[ny > 0.9].sum()); u['neg'] += float(ar[ny < -0.9].sum())
            u['draws'] += 1; u['entries'].add(ei); u['flags'].add(d['ref'] >> 24)
            up = ny > 0.9
            if key < 0x8000 and key in res['pages']:
                pw, ph, subs = res['pages'][key][3], res['pages'][key][4], res['pages'][key][5]
                cu = (UV[a[ok]] + UV[b2[ok]] + UV[c2[ok]]) / 3.0
                px, py = cu[:, 0] * pw, cu[:, 1] * ph
                for (sid, sx, sy, sw, sh) in subs:
                    m = (px >= sx) & (px < sx + sw) & (py >= sy) & (py < sy + sh)
                    if m.any():
                        sk = ('sub', key, sid); su = res['use'].setdefault(sk, dict(area=0.0, pos=0.0, neg=0.0, draws=0, entries=set(), flags=set(), ymin=1e9, ymax=-1e9))
                        su['area'] += float(ar[m].sum()); su['pos'] += float(ar[m & up].sum()); su['neg'] += float(ar[m & (ny < -0.9)].sum())
                        su['draws'] += 1; su['entries'].add(ei); su['flags'].add(d['ref'] >> 24)
            if up.any():
                ia = a[ok][up]; uv = np.concatenate([UV[ia], UV[b2[ok][up]], UV[c2[ok][up]]])
                u.setdefault('upuv', []).append((float(ar[up].sum()), float(uv[:,0].min()), float(uv[:,1].min()), float(uv[:,0].max()), float(uv[:,1].max())))
            u['ymin'] = min(u['ymin'], float(p0[:, 1].min())); u['ymax'] = max(u['ymax'], float(p0[:, 1].max()))
    except Exception as e:
        res['errors'].append((ei, repr(e)))
    if ei % 50 == 0: print(ei, len(res['use']), len(res['cuts']), len(res['pages']), flush=True)
pickle.dump(res, open('work/tmp/sod_texuse.pkl', 'wb'))
print('done: refs', len(res['use']), 'cuts', len(res['cuts']), 'pages', len(res['pages']), 'errors', len(res['errors']))
