# Upgrade an old (B2b) SDMK dump to the current geometry header: flags = 1 (light factor) and the per-light
# caster boxes, computed exactly like BridgeLink::BuildGeometry / make_dump.py. usage: upgrade_dump.py in out
import sys, struct, numpy as np
d = bytearray(open(sys.argv[1], 'rb').read())
h = struct.unpack_from('<6I', d, 0); gb = h[5]; G = 24
lc, cc, tc, fl = struct.unpack_from('<4I', d, G + 96)
lights = [struct.unpack_from('<4f', d, G + 112 + 16 * i) for i in range(lc)]
casters = [struct.unpack_from('<4I', d, G + 240 + 16 * i) for i in range(cc)]
T = np.frombuffer(bytes(d), np.float32, tc * 9, G + gb - tc * 36).reshape(-1, 3, 3).astype(np.float64)
struct.pack_into('<I', d, G + 108, 1)
off = G + 1264
for l in range(8):
    if l >= lc: struct.pack_into('<12f', d, off + 48 * l, *([0.0] * 12)); continue
    L = np.array(lights[l][:3], np.float32).astype(np.float64)
    a = np.array([0.0, 1.0, 0.0]) if abs(L[0]) > 0.9 else np.array([1.0, 0.0, 0.0])
    u = np.cross(L, a); u /= np.linalg.norm(u); v = np.cross(L, u)
    sel = [c for c in casters if (c[2] >> l) & 1]
    if not sel: struct.pack_into('<12f', d, off + 48 * l, *u, 1e30, *v, -1e30, 1e30, -1e30, -1e30, 0); continue
    P = np.concatenate([T[c[0]:c[0] + c[1]].reshape(-1, 3) for c in sel])
    pu, pv, pl = P @ u, P @ v, P @ L
    struct.pack_into('<12f', d, off + 48 * l, *u, pu.min(), *v, pu.max(), pv.min(), pv.max(), pl.max(), 0)
open(sys.argv[2], 'wb').write(d)
print('lights', lc, 'casters', cc, 'tris', tc, 'masks', [hex(c[2]) for c in casters])
