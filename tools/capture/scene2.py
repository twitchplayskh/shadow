import sys, struct, math
sys.path.insert(0, '.')
import parse_capture2 as pc
fr, nf, ev = pc.load(sys.argv[1])
frame = []
for e in ev:
    if e['tag'] == 'PRES': break
    frame.append(e)
pas = -1
for i, e in enumerate(frame):
    if e['tag'] == 'CLR ': print('CLEAR', hex(e['flags'])); continue
    if e['tag'] != 'DRAW': continue
    d = e
    if d['fvf'] == 0x1d2 and d['stencilEnable'] and d['stencilPass'] == 8:
        st = d['stride']; vb = d['vb']; n = len(vb)//st
        P = [struct.unpack_from('<3f', vb, k*st) for k in range(n)]
        good = [k for k in range(0, n-1, 2) if sum((P[k+1][j]-P[k][j])**2 for j in range(3)) > 1]
        dv = [P[good[0]+1][j]-P[good[0]][j] for j in range(3)]; L = math.sqrt(sum(x*x for x in dv))
        near = [P[k] for k in good]
        print(' VOL prim x', d['primCount'], 'vb %x mi %d' % (d['vbPtr'], d['minIndex']), 'dir', [round(x/L, 4) for x in dv], 'len', round(L), 'near bbox', [round(min(p[j] for p in near)) for j in range(3)], [round(max(p[j] for p in near)) for j in range(3)])
    elif d['fvf'] == 0x44:
        a = struct.unpack_from('<4fI', d['vb'], 0)[4] >> 24
        print(' DARKEN alpha', a, round(a/255, 4), 'blend', d['srcBlend'], d['dstBlend'], 'stencilFunc', d['stencilFunc'])
# proj / depth convention
d = next(e for e in frame if e['tag'] == 'DRAW' and e['fvf'] == 0x142)
print('proj', [round(x, 5) for x in d['proj']])
print('view', [round(x, 4) for x in d['view']])
print('env zFunc', d['zFunc'], 'vp', d['vp'])
