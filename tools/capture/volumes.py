import struct, sys, math
sys.path.insert(0, 'work/rt_analysis')
from parse_capture import load
fr, dr, ps = load(sys.argv[1])
for i, h in enumerate(dr):
    if h['fvf'] != 0x1d2 or h['stencilPass'] != 8: continue
    vb, st = h['vb'], h['stride']; nv = len(vb)//st
    P = [struct.unpack_from('<3f', vb, k*st) for k in range(nv)]
    ds = [tuple(P[k+1][j]-P[k][j] for j in range(3)) for k in range(0, nv-1, 2)]
    L = [math.sqrt(sum(c*c for c in d)) for d in ds]
    nd = [tuple(c/l for c in d) for d, l in zip(ds, L) if l > 1e-6]
    avg = tuple(sum(d[j] for d in nd)/len(nd) for j in range(3))
    spread = max(math.acos(max(-1,min(1,sum(a*b for a,b in zip(d, avg))/math.sqrt(sum(c*c for c in avg))))) for d in nd)
    near = P[0::2]
    mn = [min(p[j] for p in near) for j in range(3)]; mx = [max(p[j] for p in near) for j in range(3)]
    idx = struct.unpack('<%dH' % (len(h['ib'])//2), h['ib'])
    print(i, 'pass', h['pass'], 'nv', nv, 'len %.1f..%.1f' % (min(L), max(L)), 'dir', tuple(round(c,4) for c in avg), 'spread %.3f deg' % math.degrees(spread),
          'near bbox', [round(x) for x in mn], [round(x) for x in mx], 'idx[:12]', idx[:12], 'max idx', max(idx), 'degenerate restarts', sum(1 for a,b in zip(idx, idx[1:]) if a==b))
print('--- filtered')
for i, h in enumerate(dr):
    if h['fvf'] != 0x1d2 or h['stencilPass'] != 8: continue
    vb, st = h['vb'], h['stride']; nv = len(vb)//st
    P = [struct.unpack_from('<3f', vb, k*st) for k in range(nv)]
    good = [k for k in range(0, nv-1, 2) if sum((P[k+1][j]-P[k][j])**2 for j in range(3)) > 1]
    zero = [k for k in range(0, nv-1, 2) if k not in good]
    near = [P[k] for k in good]
    mn = [min(p[j] for p in near) for j in range(3)]; mx = [max(p[j] for p in near) for j in range(3)]
    print(i, 'pairs', len(good), 'zero pairs', len(zero), 'first zero', zero[:4], [P[k] for k in zero[:2]], 'near bbox', [round(x) for x in mn], [round(x) for x in mx])
