import os
import sys, pickle, numpy as np
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from wad import *
f = pickle.load(open('shadow_cands.pkl', 'rb'))
use = pickle.load(open('sod_texuse.pkl', 'rb'))
pages = use['pages']
out = {}
for key, lst in sorted(f.items()):
    if key == 0 or key not in pages: continue
    ei, c, sz, pw, ph, subs = pages[key]
    P = np.array(compose_page(ccc_entry(ei), c, sz)[0]); H, W = P.shape[:2]
    for (u0, v0, u1, v1, e, nv, fl, _, y) in lst:
        if not (0 <= u0 < u1 <= 1.001 and 0 <= v0 <= v1 <= 1.001): continue
        reg = P[int(v0 * H):max(int(v1 * H), int(v0 * H) + 1), int(u0 * W):max(int(u1 * W), int(u0 * W) + 1)]
        al = reg[..., 3].astype(float) / 255; rgb = reg[..., :3].astype(float)
        lum = float((rgb.mean(axis=2) * al).sum() / max(al.sum(), 1e-6)); sat = float(np.ptp(rgb, axis=2).mean())
        k = (key, round(u0, 3), round(v0, 3), round(u1, 3), round(v1, 3))
        out.setdefault(k, dict(lum=lum, alpha=float(al.mean()), sat=sat, entries=set(), nv=0))
        out[k]['entries'].add(e); out[k]['nv'] = max(out[k]['nv'], nv)
for k, v in sorted(out.items(), key=lambda kv: kv[1]['lum']):
    print(k, 'lum %.0f alpha %.2f sat %.0f nv %d entries %s' % (v['lum'], v['alpha'], v['sat'], v['nv'], sorted(v['entries'])[:6]))
pickle.dump(out, open('shadow_regions.pkl', 'wb'))
