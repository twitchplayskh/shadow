# Parser for SoD-RT frame captures (sod_rt_capture_N.bin, format SODCAP01)
import struct, sys
HDR = '<IIiIIIIiIIIIIIIIIIIIIIIIIIIII' + '16f16f16f'
HSZ = struct.calcsize(HDR)
FIELDS = ('magic api pass fvf stride primType primCount baseVertex minIndex numVertices startIndex indexBits cull '
          'stencilEnable stencilPass stencilFail stencilZFail stencilFunc zEnable zWrite zFunc colorWrite alphaBlend '
          'srcBlend dstBlend lighting vs ccwStencilPass twoSided').split()
def load(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'SODCAP01', 'bad magic'
    frame, ndraws = struct.unpack_from('<II', d, 8)
    o = 16; draws = []
    for _ in range(ndraws):
        v = struct.unpack_from(HDR, d, o); o += HSZ
        h = dict(zip(FIELDS, v[:29])); h['world'] = v[29:45]; h['view'] = v[45:61]; h['proj'] = v[61:77]
        assert h['magic'] == 0x57415244, 'draw magic at %d' % o
        n = struct.unpack_from('<I', d, o)[0]; o += 4; h['vb'] = d[o:o+n]; o += n
        n = struct.unpack_from('<I', d, o)[0]; o += 4; h['ib'] = d[o:o+n]; o += n
        draws.append(h)
    passes = []
    if o + 8 <= len(d) and struct.unpack_from('<I', d, o)[0] == 0x53534150:
        n = struct.unpack_from('<I', d, o + 4)[0]; o += 8
        for _ in range(n): passes.append(struct.unpack_from('<fI', d, o)); o += 8
    return frame, draws, passes
if __name__ == '__main__':
    fr, dr, ps = load(sys.argv[1])
    print('frame', fr, 'draws', len(dr), 'passes', ps)
    for i, h in enumerate(dr):
        print(i, 'api', h['api'], 'pass', h['pass'], 'fvf %x' % h['fvf'], 'stride', h['stride'], 'prim', h['primType'], 'x', h['primCount'],
              'verts', h['numVertices'], 'vb', len(h['vb']), 'ib', len(h['ib']), 'cull', h['cull'], 'st', h['stencilEnable'], h['stencilPass'], h['stencilZFail'])
