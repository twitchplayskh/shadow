# SoDFix 0.8.1 menu art (replaces make_rt_aa_art.py). Starts from the atlas as shipped up to 0.9.6.
#  - image 7 (tex 819) is extended from 512x400 to 512x512 rows: its record already declares a 512x512
#    texture (+0x04), so the game's texture size and every existing rect's UVs stay the same
#  - every new rect is checked against ALL game rects of its texture (0.10.0 put art inside rects 0x40,
#    0x4D, 0x4E - visible as a second help line above "UP and DOWN")
import sys, json, struct; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from tbn2 import *
import itemfont as F
import numpy as np, pefile
from PIL import Image, ImageDraw, ImageFont
F.G['G']=(193,210,443,473)
F.G['X']=(116,131,0,32)                      # from the game's EXIT label
SRC='work/assets/option_e_v096_backup.tbn2'
OUT='../../sodfix/data/option_e.tbn2'
pe=pefile.PE('work/shadow.exe'); img=pe.get_memory_mapped_image()
game={}
for i in range(0x5d):
    t,p,u,v,w,h=struct.unpack_from('<HHhhhh',img,0x939450-0x400000+i*12)
    game.setdefault(t,[]).append((i,u,v,w,h))

# ---- extend image 7 to 512 rows
d=bytearray(open(SRC,'rb').read())
r7=parse(bytes(d))[7]; o=r7['off']
assert struct.unpack_from('<HHHH',d,o+4)==(512,512,512,512) and r7['h']==400 and r7['bpp']==8
NEWH=512; extra=(NEWH-r7['h'])*512
struct.pack_into('<H',d,o+0x2a,NEWH)
assert struct.unpack_from('<I',d,o+0x34)[0]==0x10+r7['npix']
struct.pack_into('<I',d,o+0x34,0x10+r7['npix']+extra)
d[r7['px']+r7['npix']:r7['px']+r7['npix']]=bytes(extra)
recs=parse(bytes(d)); r7=recs[7]; r8=recs[8]
assert r7['h']==NEWH and len(recs)==10
ix7=indices(bytes(d),r7); ix8=indices(bytes(d),r8)
W=512
pal7=palette(bytes(d),r7)
a2i={}
for i,c in enumerate(pal7):
    if c[:3]==(255,255,255) and c[3] not in a2i: a2i[c[3]]=i
to7=lambda a: a2i[min(a2i,key=lambda q:abs(q-a))]

ours={819:[],887:[]}
def overlaps(tex,u,v,w,h,skip=()):
    hit=[i for (i,gu,gv,gw,gh) in game[tex] if u<gu+gw and gu<u+w and v<gv+gh and gv<v+h]
    hit+=[n for (n,(ou,ov,ow,oh)) in ours[tex] if n not in skip and u<ou+ow and ou<u+w and v<ov+oh and ov<v+h]
    return hit
def erase(ix,u,v,w,h):
    for y in range(v,v+h):
        for x in range(u,u+w): ix[y*W+x]=0
def place(ix,tex,name,cols,u,v,h,pad,yoff):
    wdt=len(cols)+2*pad
    assert u+wdt<=W, (name,'too wide')
    assert not overlaps(tex,u,v,wdt,h), (name,'overlaps',overlaps(tex,u,v,wdt,h))
    for y in range(v,v+h):
        for x in range(u,u+wdt): assert ix[y*W+x]==0, (name,'ink',x,y)
    for i,col in enumerate(cols):
        for y,val in enumerate(col[yoff:yoff+h]):
            if val: ix[(v+y)*W+u+pad+i]=val
    ours[tex].append((name,(u,v,wdt,h)))
    return dict(tex=tex,u=u,v=v,w=wdt,h=h,yoff=yoff)
def keep(tex,name,u,v,w,h):       # existing art of ours: must not overlap game rects either
    assert not overlaps(tex,u,v,w,h), (name,'existing art overlaps',overlaps(tex,u,v,w,h))
    ours[tex].append((name,(u,v,w,h)))
def crop_rows(cols):
    rows=[y for y in range(len(cols[0])) if any(c[y] for c in cols)]
    return max(0,rows[0]-1), min(len(cols[0]),rows[-1]+2)
def item7(cols): return [[to7(F.A[v]) for v in c] for c in cols]
def hyphen():
    h=F.glyph('H'); mid=len(h)//2
    bar=[y for y in range(F.LH) if F.A[h[mid][y]]>=100]
    col=[0]*F.LH
    for y in bar: col[y]=h[mid][y]
    return [[0]*F.LH]+[list(col) for _ in range(9)]+[[0]*F.LH]
font=ImageFont.truetype('/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf',19)
def helpcols(text):
    tw=int(font.getlength(text))+2
    im=Image.new('L',(tw,20),0); dr=ImageDraw.Draw(im)
    dr.text((1,-font.getbbox('C')[1]+1),text,fill=255,font=font)
    top=max(a2i)
    return [[to7(im.getpixel((x,y))*top//255) if im.getpixel((x,y))>20 else 0 for y in range(20)] for x in range(tw)]

out={}
# ---- image 7: existing art kept (checked), RAY TRACING title replaced in place
mode=json.load(open('work/assets/mode_rects.json'))
for k,r in mode.items():
    if k!='WINDOWED': keep(819,k,r['u'],r['v'],r['w'],r['h'])
rt=json.load(open('work/assets/rt_row_rects.json'))
keep(819,'ULTRA',rt['ULTRA']['u'],rt['ULTRA']['v'],rt['ULTRA']['w'],rt['ULTRA']['h'])
hr=rt['help_rt']
erase(ix7,hr['u'],hr['v']+18,hr['w'],hr['h']-18)   # ink only in rows 0..15; 18-row rect stays clear of rect 0x4F
keep(819,'help_rt',hr['u'],hr['v'],hr['w'],18); out['help_rt']=dict(tex=819,u=hr['u'],v=hr['v'],w=hr['w'],h=18,yoff=0)
o_=rt['RAY TRACING']; erase(ix7,o_['u'],o_['v'],o_['w'],o_['h'])
t=item7(F.compose('RT SHADOW CASTING')); a,b=crop_rows(t); out['RT SHADOW CASTING']=place(ix7,819,'RT SHADOW CASTING',t,234,352,b-a,5,a)
t=item7(F.compose('SMAA')); a,b=crop_rows(t); out['SMAA']=place(ix7,819,'SMAA',t,300,376,b-a,5,a)
# ---- new rows 400..511 (no game rect reaches them): three 24-row bands of labels, two 20-row bands of help lines
aa=item7(F.compose('ANTI')+hyphen()+F.compose('ALIASING'))
labels={'ANTI-ALIASING':aa}
for name in ['TEXTURE FILTERING','STANDARD','TRILINEAR','ANISOTROPIC','CINEMATIC BARS','LAYOUT','AMBIENT OCCLUSION']:
    labels[name]=item7(F.compose(name))
helps={'help_aa':'Smooth jagged edges','help_tf':'Reduce texture shimmer','help_cb':'Black bars in cutscenes','help_kb':'Keyboard layout','help_ao':'Shade corners and contact points'}
# (0.12: CLASSIC / MODERN moved to image 8 to make room for AMBIENT OCCLUSION)
bands=[(400,24,['TEXTURE FILTERING','ANTI-ALIASING']),(424,24,['CINEMATIC BARS','ANISOTROPIC','STANDARD']),
       (448,24,['TRILINEAR','LAYOUT','AMBIENT OCCLUSION']),(472,20,['help_tf','help_cb','help_kb']),(492,20,['help_aa','help_ao'])]
for y0,h,names in bands:
    x=0
    for name in names:
        if name in labels:
            t=labels[name]; a,b=crop_rows(t); assert b-a<=h,(name,b-a)
            out[name]=place(ix7,819,name,t,x,y0,b-a,5,a)
        else:
            c=helpcols(helps[name]); out[name]=place(ix7,819,name,c,x,y0,20,2,0)
        x+=out[name]['w']+1
    assert x<=W+1,(y0,x)
set_indices(d,r7,ix7)

# ---- image 8 (tex 887): our resolution labels; 1440 x 1080 overlapped game rect 0x22 -> move it
res=json.load(open('work/assets/new_rects.json'))
busy8=np.zeros((512,512),bool)
for (i,u,v,w,h) in game[887]: busy8[max(v,0):v+h,max(u,0):u+w]=True
A8=np.array([c[3] for c in palette(bytes(d),r8)]); arr8=np.array(ix8).reshape(512,512)
busy8|=A8[arr8]>0
for r in res:
    busy8[r['v']:r['v']+r['h'],r['u']:r['u']+r['w']]=True
newres=[]
for r in res:
    if not overlaps(887,r['u'],r['v'],r['w'],r['h']):
        keep(887,r['text'],r['u'],r['v'],r['w'],r['h']); newres.append(r); continue
    spot=None
    for yy in range(0,512-r['h']+1,2):
        for xx in range(0,512-r['w']+1,2):
            if not busy8[yy:yy+r['h'],xx:xx+r['w']].any(): spot=(xx,yy); break
        if spot: break
    assert spot, r
    blk=arr8[r['v']:r['v']+r['h'],r['u']:r['u']+r['w']].copy()
    arr8[r['v']:r['v']+r['h'],r['u']:r['u']+r['w']]=0
    # restore whatever game ink the game rect expects there: the old label sat in empty texture (asserted
    # empty when it was made), so clearing it returns the original pixels
    arr8[spot[1]:spot[1]+r['h'],spot[0]:spot[0]+r['w']]=blk
    busy8[spot[1]:spot[1]+r['h'],spot[0]:spot[0]+r['w']]=True
    keep(887,r['text'],spot[0],spot[1],r['w'],r['h'])
    nr=dict(r); nr['u'],nr['v']=spot; newres.append(nr); print('moved',r['text'],'->',spot)
# CLASSIC / MODERN in image 8: the item font's own pixels (same look as KEYBOARD / JOYPAD next to them)
def place8(name,cols,u,v,h,pad,yoff):
    wdt=len(cols)+2*pad
    assert not overlaps(887,u,v,wdt,h), (name,'overlaps',overlaps(887,u,v,wdt,h))
    assert not busy8[v:v+h,u:u+wdt].any(), (name,'busy')
    for i,col in enumerate(cols):
        for y,val in enumerate(col[yoff:yoff+h]):
            if val: arr8[v+y,u+pad+i]=val
    busy8[v:v+h,u:u+wdt]=True; ours[887].append((name,(u,v,wdt,h)))
    return dict(tex=887,u=u,v=v,w=wdt,h=h,yoff=yoff)
for name,(u,v) in [('CLASSIC',(420,440)),('MODERN',(366,488))]:
    t=F.compose(name); a,b=crop_rows(t); out[name]=place8(name,t,u,v,b-a,5,a)
set_indices(d,r8,list(arr8.reshape(-1)))
# ---- image 10 (new, 0.13): a fresh 512x512 sheet for options added from 0.13 on. The loader (0x74A990) walks every
# group until a zero id, so an appended group is loaded like the others. Texture 4999 / palette 21999: unused anywhere
# (image ids go up to 4606; table 0xAB3998 holds -16384..16383). Copy of image 7's group (same palette), pixels cleared.
NEWTEX, NEWPAL = 4999, 21999
recs=parse(bytes(d)); r7=recs[7]; g0=r7['off']; gend=r7['pal']+(4<<r7['bpp'])
grp=bytearray(d[g0:gend])
struct.pack_into('<I',grp,0,NEWTEX)
struct.pack_into('<I',grp,r7['pal']-0x20-g0,NEWPAL)
grp[r7['px']-g0:r7['px']-g0+r7['npix']]=bytes(r7['npix'])
ix10=[0]*(512*512); ours[NEWTEX]=[]; game[NEWTEX]=[]
def place10(name,cols,u,v,h,pad,yoff):
    wdt=len(cols)+2*pad
    assert u+wdt<=W and not overlaps(NEWTEX,u,v,wdt,h), (name,'no room')
    for i,col in enumerate(cols):
        for y,val in enumerate(col[yoff:yoff+h]):
            if val: ix10[(v+y)*W+u+pad+i]=val
    ours[NEWTEX].append((name,(u,v,wdt,h)))
    return dict(tex=NEWTEX,u=u,v=v,w=wdt,h=h,yoff=yoff)
t=item7(F.compose('REFLECTIONS')); a_,b_=crop_rows(t); out['REFLECTIONS']=place10('REFLECTIONS',t,0,0,b_-a_,5,a_)
out['help_ssr']=place10('help_ssr',helpcols('Reflections on polished floors'),0,24,20,2,0)
# 0.13.2: DRAW DISTANCE EXTRA, full 32-row cell like the game's LOW / MIDDLE / HIGH rects (0x59..0x5B)
out['EXTRA']=place10('EXTRA',item7(F.compose('EXTRA')),0,48,32,1,0)
grp[r7['px']-g0:r7['px']-g0+r7['npix']]=bytes(ix10)
last=parse(bytes(d))[-1]; endlast=last['pal']+(4<<last['bpp'])
assert struct.unpack_from('<I',d,endlast)[0]==0, 'no terminator after the last group'
d[endlast:endlast]=grp
assert len(parse(bytes(d)))==11
open(OUT,'wb').write(d)
json.dump(out,open('work/assets/menu_v2_rects.json','w'),indent=1)
json.dump(newres,open('work/assets/new_rects_v2.json','w'),indent=1)
print(json.dumps(out)); print(newres)
im=decode(bytes(d),r7); bg=Image.new('RGBA',im.size,(40,40,40,255)); bg.alpha_composite(im); bg.save('work/tmp/opt7_v2.png')
im=decode(bytes(d),r8); bg=Image.new('RGBA',im.size,(120,100,80,255)); bg.alpha_composite(im); bg.save('work/tmp/opt8_v2.png')

# ---- C header for SoDFix: every rect of ours
with open('../../sodfix/src/menu_art.h','w') as f:
    f.write('// generated by scripts/make_menu_art_v2.py - SoDFix menu art in option_e.tbn2 (tex, pal, u, v, w, h; yoff)\n#pragma once\n')
    pal={819:13226,887:13991,4999:21999}
    for k,r in out.items():
        n='kArt_'+''.join(ch if ch.isalnum() else '_' for ch in k)
        f.write('static const SpriteRect %s = { %d, %d, %d, %d, %d, %d };   // yoff %d\n'%(n,r['tex'],pal[r['tex']],r['u'],r['v'],r['w'],r['h'],r['yoff']))
        f.write('static const int %s_yoff = %d;\n'%(n,r['yoff']))
