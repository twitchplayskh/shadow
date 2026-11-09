import os
# flat, semi-transparent page-textured quads at floor level whose texture region is dark: the baked blob shadows
import sys, struct, pickle, numpy as np
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from wad import *
found = {}   # (page) -> list of (u0,v0,u1,v1, entry, nv, flags, darkness)
stats = dict(cands=0)
pagecache = {}
for ei in range(nentries()):
    try:
        x = ccc_entry(ei)
        pages = {}
        for c, sz in tex_chunks(x):
            pid = struct.unpack_from('<I', x, c + 0x20)[0]; pages[pid] = (c, sz)
        draws, vs, isec = draw_records(x)
        if not draws: continue
        VV = np.frombuffer(x, np.float32, (isec - vs) // 4, vs).reshape(-1, 6)
        C = np.frombuffer(x, np.uint32, (isec - vs) // 4, vs).reshape(-1, 6)[:, 3]
        for d in draws:
            key = d['ref'] & 0xFFFF
            if key >= 0x8000: continue
            idx = np.frombuffer(x, np.uint16, d['ni'], isec + 2 * (d['ib'] + d['si'])).astype(np.int64) + d['vb']
            if len(idx) < 3 or idx.max() >= len(VV): continue
            v = VV[np.unique(idx)]; a = (C[np.unique(idx)] >> 24)
            if v[:, 1].max() - v[:, 1].min() > 1.0 or a.max() >= 0x80 + 1 and a.min() >= 0xff: continue
            if a.min() >= 0xff: continue
            stats['cands'] += 1
            u0, v0, u1, v1 = float(v[:, 4].min()), float(v[:, 5].min()), float(v[:, 4].max()), float(v[:, 5].max())
            dark = None
            if key in pages:
                if (ei, key) not in pagecache:
                    pagecache.clear(); pagecache[(ei, key)] = np.array(compose_page(x, *pages[key])[0])
                P = pagecache[(ei, key)]; ph, pw = P.shape[:2]
                reg = P[int(v0 * ph):max(int(v1 * ph), int(v0 * ph) + 1), int(u0 * pw):max(int(u1 * pw), int(u0 * pw) + 1)]
                if reg.size:
                    al = reg[..., 3].astype(float) / 255
                    rgb = reg[..., :3].mean(axis=2)
                    dark = (float((rgb * al).sum() / max(al.sum(), 1e-6)), float(al.mean()))
            found.setdefault(key, []).append((round(u0, 4), round(v0, 4), round(u1, 4), round(v1, 4), ei, len(v), d['ref'] >> 24, dark, float(v[:, 1].mean())))
    except Exception as e:
        pass
    if ei % 100 == 0: print(ei, stats, len(found), flush=True)
pickle.dump(found, open('work/shadow_cands.pkl', 'wb'))
print('done', stats, len(found))
