# Parser for SoD-RT capture v2 (sod_rt_capture2_N.bin, SODCAP02): tagged event stream over 4 frames
import struct, sys
HDR1 = '<IIiIIIIiIIIIIIIIIIIIIIIIIIIII' + '16f16f16f'
F1 = ('magic api pass fvf stride primType primCount baseVertex minIndex numVertices startIndex indexBits cull '
      'stencilEnable stencilPass stencilFail stencilZFail stencilFunc zEnable zWrite zFunc colorWrite alphaBlend '
      'srcBlend dstBlend lighting vs ccwStencilPass twoSided').split()
SURF = '8I'; SF = 'ptr w h fmt tex usage pool backbuf'.split()
EXT = '<' + SURF + SURF + '4I4I4I' + '4I2f' + '9I'
def surf(v): return dict(zip(SF, v))
def load(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'SODCAP02', 'bad magic'
    frame, nframes, nrec = struct.unpack_from('<III', d, 8)
    o = 20; ev = []
    while o + 8 <= len(d):
        tag, ln = struct.unpack_from('<II', d, o); o += 8; p = d[o:o+ln]; o += ln
        t = struct.pack('<I', tag).decode('latin1')
        e = {'tag': t}
        if t == 'DRAW':
            h1 = struct.calcsize(HDR1); v = struct.unpack_from(HDR1, p, 0)
            e.update(zip(F1, v[:29])); e['world'] = v[29:45]; e['view'] = v[45:61]; e['proj'] = v[61:77]
            x = struct.unpack_from(EXT, p, h1); q = h1 + struct.calcsize(EXT)
            e['rt0'] = surf(x[0:8]); e['ds'] = surf(x[8:16]); e['tex'] = x[16:20]; e['texW'] = x[20:24]; e['texH'] = x[24:28]
            e['vp'] = x[28:34]; e['vbPtr'], e['vbOffset'], e['ibPtr'], e['zBias'], e['alphaTest'], e['alphaRef'], e['alphaFunc'], e['fog'], e['scissor'] = x[34:43]
            n = struct.unpack_from('<I', p, q)[0]; q += 4; e['vb'] = p[q:q+n]; q += n
            n = struct.unpack_from('<I', p, q)[0]; q += 4; e['ib'] = p[q:q+n]; q += n
        elif t == 'CLR ':
            v = struct.unpack_from('<IIIfI' + SURF + SURF, p); e.update(count=v[0], flags=v[1], color=v[2], z=v[3], stencil=v[4], rt0=surf(v[5:13]), ds=surf(v[13:21]))
        elif t == 'SRT ':
            v = struct.unpack_from('<I' + SURF, p); e.update(index=v[0], s=surf(v[1:9]))
        elif t == 'SDS ':
            e['s'] = surf(struct.unpack_from('<' + SURF, p))
        elif t == 'COPY':
            v = struct.unpack_from('<I' + SURF + SURF + '4i4iII', p); e.update(kind=v[0], src=surf(v[1:9]), dst=surf(v[9:17]), sr=v[17:21], dr=v[21:25], hasSr=v[25], hasDr=v[26])
        elif t == 'PRES':
            v = struct.unpack_from('<7I8f', p); e.update(frame=v[0], site=v[1], passes=v[2], draws=v[3], clears=v[4], rts=v[5], copies=v[6], density=v[7:7+v[2]] if v[2] <= 8 else v[7:15])
        elif t == 'UTEX':
            e['src'], e['dst'] = struct.unpack_from('<II', p)
        ev.append(e)
    return frame, nframes, ev
def sname(s): return '%x %dx%d f%d%s' % (s['ptr'], s['w'], s['h'], s['fmt'], ' BB' if s['backbuf'] else (' tex %x' % s['tex'] if s['tex'] else '')) if s['ptr'] else '-'
if __name__ == '__main__':
    fr, nf, ev = load(sys.argv[1])
    print('start frame', fr, 'frames', nf, 'events', len(ev))
    last = None; run = 0
    def flush():
        global last, run
        if last: print('   DRAW x%d %s' % (run, last))
        last = None; run = 0
    for e in ev:
        if e['tag'] == 'DRAW':
            key = 'api%d fvf %x rt %s tex0 %x(%dx%d) st %d/%d z %d%d cw %x ab %d' % (e['api'], e['fvf'], sname(e['rt0']), e['tex'][0], e['texW'][0], e['texH'][0],
                  e['stencilEnable'], e['stencilPass'], e['zEnable'], e['zWrite'], e['colorWrite'], e['alphaBlend'])
            if key == last: run += 1
            else: flush(); last = key; run = 1
            continue
        flush()
        if e['tag'] == 'CLR ': print(' CLEAR flags %x color %08x z %g st %d rt %s ds %s' % (e['flags'], e['color'], e['z'], e['stencil'], sname(e['rt0']), sname(e['ds'])))
        elif e['tag'] in ('SRT ', 'SDS '): print(' %s %s %s' % (e['tag'], e.get('index', ''), sname(e['s'])))
        elif e['tag'] == 'COPY': print(' COPY kind %d %s -> %s sr %s dr %s' % (e['kind'], sname(e['src']), sname(e['dst']), e['sr'] if e['hasSr'] else '-', e['dr'] if e['hasDr'] else '-'))
        elif e['tag'] == 'PRES': print('PRESENT frame %d site %x draws %d clears %d rts %d copies %d passes %d dens %s' % (e['frame'], e['site'], e['draws'], e['clears'], e['rts'], e['copies'], e['passes'], e['density']))
        else: print(' ' + e['tag'])
    flush()
