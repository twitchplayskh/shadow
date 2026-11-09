import os
# Shadow of Destiny PC archives: ccc.wad (rooms) = [6 u32 hdr: 0, 1, usize, stored size incl. header, 0, 0] then
# zlib blocks [u32 usize <= 0x8000][u32 csize][zlib], data 16-aligned; other wads = [u32 usize][u32 csize][zlib]
import zlib, struct, bisect, re, pickle, os
G=os.environ.get('SOD_GAME_DIR','work/game')+'/'
_cache={}
def ccc():
    if 'ccc' in _cache: return _cache['ccc']
    d=open(G+'ccc.wad','rb').read()
    idx='work/tmp/ccc.idx'
    blocks,hdrs=pickle.load(open(idx,'rb'))
    _cache['ccc']=(d,blocks,[b[0] for b in blocks],hdrs); return _cache['ccc']
def ccc_entry(i):
    d,blocks,bp,hdrs=ccc(); h,a,b,us,cs=hdrs[i]
    j=bisect.bisect_left(bp,h+24); out=bytearray()
    while j<len(blocks) and blocks[j][0]<h+cs:
        p,bu,bc=blocks[j]; out+=zlib.decompressobj().decompress(d[p:p+bc+64])[:bu]; j+=1
    assert len(out)==us,(i,len(out),us)
    return bytes(out)
def nentries(): return len(ccc()[3])
MAGICS=(0x19990802,0x20000327)
def models(x):
    """yield (offset, [(chunkoff, type, size)]) for each model block"""
    o=0x10
    while o+0x20<=len(x):
        m=struct.unpack_from('<I',x,o)[0]
        if m not in MAGICS: break
        lst=struct.unpack_from('<I',x,o+0x10)[0]
        c=o+0x10+lst; chunks=[]
        while c+8<=len(x):
            t,sz=struct.unpack_from('<II',x,c)
            if t==0: break
            chunks.append((c,t,sz)); c+=sz
        yield o,chunks
        o=(c+0x13)&~0xF

def tex_chunks(x):
    for mo,ch in models(x):
        for c,t,sz in ch:
            if t==4 and struct.unpack_from('<I',x,c+0x10)[0]==0x19990901: yield c, sz
def hdr_ok(x,o,end):
    if o+0x20>end: return False
    i,xx,yy,w,h,bpp,q,a,sz=struct.unpack_from('<IHHHHHHII',x,o)
    return 0<i<0x100000 and 0<w<=1024 and 0<h<=1024 and bpp in (4,8) and xx<2048 and yy<2048 and 0<sz<0x400000
def tex_entries(x, c, csize):
    """type-4 chunk (magic 0x19990901): page header (count at +0x1c) then entries [id, x, y, w, h, bpp, q, a, sz, 12 bytes] + pixels.
    The pixel block length isn't a clean function of the header: take stride*h and look for the next header nearby."""
    end=c+csize; o=c+0x10+0x30; out=[]
    count=struct.unpack_from('<H',x,c+0x10+0x1c)[0]
    while len(out)<count and hdr_ok(x,o,end):
        i,xx,yy,w,h,bpp,q,a,sz=struct.unpack_from('<IHHHHHHII',x,o)
        stride=((w+3)&~3) if bpp==8 else ((w+7)&~7)//2
        L=stride*h; px=o+0x20; nxt=px+((sz-4)&~15)
        out.append(dict(off=o,id=i,x=xx,y=yy,w=w,h=h,bpp=bpp,q=q,a=a,sz=sz,px=px,n=L)); o=nxt
    # palettes follow: first palette header starts with the first entry's id
    if out:
        k=x.find(struct.pack('<I',out[0]['id']),o-64,end)
        while k>=0 and struct.unpack_from('<H',x,k+4)[0] not in (4,8): k=x.find(struct.pack('<I',out[0]['id']),k+4,end)
        if k>=0: o=k
    return out, o
def pal_entries(x, o, end):
    out={}
    while o+0x20<=end:
        i,bpp,q=struct.unpack_from('<IHH',x,o); n1,sz=struct.unpack_from('<II',x,o+0x10)
        if bpp not in (4,8) or n1 not in (64,1024): break
        out[i]=(o+0x20,n1); o+=0x20+n1
    return out, o
import numpy as np
def unswizzle(p):          # PS2 CSM1: in every 32 entries, 8..15 <-> 16..23
    p=p.copy().reshape(-1,32,4) if len(p)==256 else p
    if p.ndim==3: p[:,8:16],p[:,16:24]=p[:,16:24].copy(),p[:,8:16].copy(); p=p.reshape(256,4)
    return p
def decode_tex(x, e, pal, swz=True):
    from PIL import Image
    stride=(e['w']+3)&~3 if e['bpp']==8 else ((e['w']+7)&~7)//2
    raw=np.frombuffer(x,np.uint8,stride*e['h'],e['px']).reshape(e['h'],stride)
    po,n1=pal[e['id']]; P=np.frombuffer(x,np.uint8,n1,po).reshape(-1,4)[:,[2,1,0,3]]
    if swz and len(P)==256: P=unswizzle(P)
    if e['bpp']==8: idx=raw[:,:e['w']]
    else:
        idx=np.stack([raw&15,raw>>4],-1).reshape(e['h'],-1)[:,:e['w']]
    rgba=P[idx].copy()
    a=rgba[...,3].astype(int); rgba[...,3]=np.clip(a*2,0,255) if a.max()<=128 else a
    return Image.fromarray(rgba,'RGBA')

def mbn_entries():
    if 'mbn' in _cache: return _cache['mbn']
    d=open(G+'mbn.wad','rb').read(); o=0; ents=[]
    while o+8<=len(d):
        us,cs=struct.unpack_from('<II',d,o)
        if cs==0 or d[o+8]!=0x78: break
        ents.append((o+8,cs,us)); o+=8+cs
    _cache['mbn']=(d,ents); return _cache['mbn']
def mbn_entry(i):
    d,ents=mbn_entries(); p,cs,us=ents[i]; return zlib.decompress(d[p:p+cs])
def scan_tex_chunks(x):
    k=x.find(struct.pack('<I',0x19990901))
    while k>=0:
        c=k-0x10
        if c>=0:
            t,sz=struct.unpack_from('<II',x,c)
            if t==4 and 0<sz<=len(x)-c: yield c,sz
            elif 0<t<=len(x)-c: yield c,t            # mbn layout: [size][type ...]
        k=x.find(struct.pack('<I',0x19990901),k+4)

def compose_page(x, c, sz):
    from PIL import Image
    pw,ph=struct.unpack_from('<HH',x,c+0x10+0x14)
    e,o=tex_entries(x,c,sz); pal,_=pal_entries(x,o,c+sz)
    page=Image.new('RGBA',(pw,ph))
    for v in e:
        if v['id'] in pal: page.paste(decode_tex(x,v,pal,False),(v['x'],v['y']))
    return page, e
def cut_records(x, page_ids):
    """16-byte records {s16 page, s16 slot (<0), u16 x, y, w, h, u16 flags, 0}: sub-textures cut from pages at load"""
    out=[]; seen=set()
    for pid in page_ids:
        b=struct.pack('<h',pid); k=x.find(b)
        while k>=0:
            if k%2==0 and k+16<=len(x):
                pg,sl,xx,yy,w,h,f,z=struct.unpack_from('<hhHHHHHH',x,k)
                if pg==pid and sl<0 and 0<w<=1024 and 0<h<=1024 and z==0 and k not in seen:
                    seen.add(k); out.append(dict(off=k,page=pg,slot=sl,x=xx,y=yy,w=w,h=h,flags=f))
            k=x.find(b,k+1)
    return out

def draw_records(x):
    """room draws: [nv, nidx, minIndex, startIndex, ref, 0, u16 1, u16 3 ...] with the object's (vertexBase, indexBase)
    taken from the nearest preceding u32 pair for which every index of the strip lies in [min, min + nv)"""
    import bisect
    vs,isec=struct.unpack_from('<II',x,0)
    if not (0x10 < vs < isec <= len(x)): return [], vs, isec
    nvert=(isec-vs)//24; nidxt=(len(x)-isec)//2
    sig=struct.pack('<HH',1,3); recs=[]; k=x.find(sig)
    while 0<=k<vs:
        if k>=0x18 and k%4==0:
            nv,ni,mn,si,ref,z=struct.unpack_from('<6I',x,k-0x18)
            if z==0 and 3<=ni<65536 and 1<=nv<65536 and mn<65536 and si<400000: recs.append((k-0x18,nv,ni,mn,si,ref))
        k=x.find(sig,k+2)
    pairs=[]
    for o in range(4,vs-8,4):
        a,b=struct.unpack_from('<II',x,o)
        if 0<=a<nvert and 0<=b<nidxt and (a or b): pairs.append((o,a,b))
    po=[p[0] for p in pairs]; out=[]
    for (o,nv,ni,mn,si,ref) in recs:
        j=bisect.bisect_left(po,o)-1; good=None
        while j>=0 and o-po[j]<0x4000:
            _,vb,ib=pairs[j]
            if vb+mn+nv<=nvert and isec+2*(ib+si+ni)<=len(x):
                idx=struct.unpack_from('<%dH'%ni,x,isec+2*(ib+si))
                if all(mn<=v<mn+nv for v in idx): good=(vb,ib); break
            j-=1
        if good: out.append(dict(off=o,nv=nv,ni=ni,min=mn,si=si,ref=ref,vb=good[0],ib=good[1]))
    return out, vs, isec
