import struct, sys
sys.path.insert(0, 'work/rt_analysis')
from parse_capture import load
fr, dr, ps = load(sys.argv[1])
def mat(m): return [m[i*4:i*4+4] for i in range(4)]
for i in (0, 4, 8, 17, 18):
    h = dr[i]
    print('--- draw', i, {k: h[k] for k in ('fvf','zEnable','zWrite','zFunc','alphaBlend','srcBlend','dstBlend','lighting','twoSided','ccwStencilPass','stencilFunc','baseVertex','minIndex','startIndex','indexBits')})
    for n in ('world','view','proj'):
        print(n, [tuple(round(x,4) for x in r) for r in mat(h[n])])
    vb = h['vb']; st = h['stride']
    nv = len(vb)//st
    for k in range(min(nv, 6)):
        if h['fvf'] == 0x1d2:
            x,y,z,nx,ny,nz,dif,spe,u,v = struct.unpack_from('<6fII2f', vb, k*st)
            print(' v', k, (round(x,3),round(y,3),round(z,3)), 'n', (round(nx,3),round(ny,3),round(nz,3)), 'dif %08x spe %08x' % (dif,spe), 'uv', (round(u,3),round(v,3)))
        elif h['fvf'] == 0x44:
            print(' v', k, struct.unpack_from('<4fI', vb, k*st))
        else:
            print(' v', k, struct.unpack_from('<4fI2f', vb, k*st))
