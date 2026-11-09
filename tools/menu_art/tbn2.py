# Shadow of Destiny PC .tbn2 texture container (decoder/encoder)
# record: 0x40 header | pixels (w*h*bpp/8) | 0x20 palette header | palette (BGRA, 1<<bpp entries)
#   hdr+0x00 u32 image id, +0x28 u16 w, +0x2a u16 h, +0x2c u32 bpp (4/8), +0x34 u32 0x10+pixel bytes
#   palhdr+0x00 u32 palette id
import struct, sys
from PIL import Image

def parse(d):
    recs = []; o = 0x10
    while o + 0x40 <= len(d):
        if d[o+0x10:o+0x14] != b'\x34\x12\x34\x12': break
        iid = struct.unpack_from('<I', d, o)[0]
        w, h, bpp = struct.unpack_from('<HHI', d, o+0x28)
        n = w*h*bpp//8; po = o+0x40+n
        pid = struct.unpack_from('<I', d, po)[0]
        recs.append(dict(off=o, id=iid, pal_id=pid, w=w, h=h, bpp=bpp, px=o+0x40, pal=po+0x20, npix=n))
        o = po + 0x20 + (4 << bpp)
    return recs

def palette(d, r):
    p = d[r['pal']:r['pal'] + (4 << r['bpp'])]
    return [(p[i*4+2], p[i*4+1], p[i*4], p[i*4+3]) for i in range(1 << r['bpp'])]

def indices(d, r):
    px = d[r['px']:r['px']+r['npix']]
    if r['bpp'] == 8: return list(px)
    out = []
    for b in px: out += [b & 15, b >> 4]
    return out

def set_indices(buf, r, idx):
    if r['bpp'] == 8:
        buf[r['px']:r['px']+r['npix']] = bytes(idx)
    else:
        buf[r['px']:r['px']+r['npix']] = bytes((idx[i] & 15) | ((idx[i+1] & 15) << 4) for i in range(0, len(idx), 2))

def decode(d, r):
    pal = palette(d, r); im = Image.new('RGBA', (r['w'], r['h']))
    im.putdata([pal[i] for i in indices(d, r)]); return im

if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    for i, r in enumerate(parse(d)):
        print(i, {k: (hex(v) if k in ('off','px','pal') else v) for k, v in r.items()})
        if len(sys.argv) > 2: decode(d, r).save('%s_%d.png' % (sys.argv[2], i))
