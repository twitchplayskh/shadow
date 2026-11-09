import sys, struct, math
sys.path.insert(0, '.')
import parse_capture2 as pc
fr, nf, ev = pc.load(sys.argv[1])
frame = []
for e in ev:
    if e['tag'] == 'PRES': break
    frame.append(e)
draws = [e for e in frame if e['tag'] == 'DRAW']
def isI(m): return all(abs(m[i] - (1.0 if i % 5 == 0 else 0.0)) < 1e-6 for i in range(16))
from collections import Counter
c = Counter((d['api'], d['fvf'], d['stencilEnable'], d['zFunc'], d['zWrite'], d['cull'], d['alphaBlend'], d['lighting']) for d in draws)
for k, v in c.items(): print('api fvf st zfunc zwrite cull ab light', k, 'x', v)
def bbox(d):
    st = d['stride']; vb = d['vb']; n = len(vb) // st
    if n == 0: return None
    P = [struct.unpack_from('<3f', vb, k * st) for k in range(n)]
    W = d['world']
    def xf(p): return tuple(p[0]*W[0+j] + p[1]*W[4+j] + p[2]*W[8+j] + W[12+j] for j in range(3))
    Q = [xf(p) for p in P if abs(p[0]) + abs(p[1]) + abs(p[2]) > 0]
    if not Q: return None
    return [round(min(q[j] for q in Q)) for j in range(3)], [round(max(q[j] for q in Q)) for j in range(3)], n
print('--- character-ish draws (fvf 1d2, no stencil)')
for i, d in enumerate(draws):
    if d['fvf'] == 0x1d2 and not d['stencilEnable']:
        print(i, 'prim', d['primType'], 'x', d['primCount'], 'nv', d['numVertices'], 'worldI', isI(d['world']), 'tex %x' % d['tex'][0], 'bbox', bbox(d), 'vb %x+%d ib %x start %d' % (d['vbPtr'], d['vbOffset'], d['ibPtr'], d['startIndex']), 'bv', d['baseVertex'], 'mi', d['minIndex'])
print('--- env draws (fvf 142 api1) first 6')
for i, d in enumerate(draws):
    if d['fvf'] == 0x142 and d['api'] == 1 and i < 40:
        print(i, 'prim', d['primType'], 'x', d['primCount'], 'worldI', isI(d['world']), 'bbox', bbox(d))
