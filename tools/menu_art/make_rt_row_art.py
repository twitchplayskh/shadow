import sys, json, struct; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from tbn2 import *
import helpfont as H, itemfont as F
import pefile
F.G['G']=(193,210,443,473)
pe=pefile.PE('work/shadow.exe'); img=pe.get_memory_mapped_image()
def table_rects(tex):
    r=[]
    for i in range(0x5d):
        t,p,u,v,w,h=struct.unpack_from('<HHhhhh',img,0x939450-0x400000+i*12)
        if t==tex: r.append((u,v,w,h))
    return r
P=json.load(open('work/assets/mode_rects.json'))
ours7=[(r['u'],r['v'],r['w'],r['h']) for k,r in P.items() if r['tex']==819 and k!='WINDOWED']
buf=bytearray(open('../../sodfix/data/option_e.tbn2','rb').read())
recs=parse(bytes(buf)); r7=recs[7]; ix=indices(bytes(buf),r7); W=512
pal7=palette(bytes(buf),r7); A7=[c[3] for c in pal7]
a2i={}
for i,c in enumerate(pal7):
    if c[:3]==(255,255,255) and c[3] not in a2i: a2i[c[3]]=i
to7=lambda a: a2i[min(a2i,key=lambda q:abs(q-a))]
# WINDOWED art is no longer used: clear it
w=P['WINDOWED']
for y in range(w['v'],w['v']+w['h']):
    for x in range(w['u'],w['u']+w['w']): ix[y*W+x]=0
def inside(x,y,rs): return any(u<=x<u+ww and v<=y<v+hh for u,v,ww,hh in rs)
def place(name,cols,u,v,h,pad,yoff):
    wdt=len(cols)+2*pad; rect=(u,v,wdt,h)
    others=table_rects(819)+ours7
    for y in range(v,v+h):                      # existing ink inside our rect?
        for x in range(u,u+wdt): assert ix[y*W+x]==0, (name,'foreign ink',x,y)
    for i,col in enumerate(cols):
        for y,val in enumerate(col[yoff:yoff+h]):
            if val:
                X,Y=u+pad+i,v+y
                assert not inside(X,Y,others), (name,'our ink in other rect',X,Y)
                ix[Y*W+X]=val
    ours7.append(rect); return dict(tex=819,u=u,v=v,w=wdt,h=h,yoff=yoff)
def crop_rows(cols):
    rows=[y for y in range(len(cols[0])) if any(c[y] for c in cols)]
    return max(0,rows[0]-1), min(len(cols[0]),rows[-1]+2)
out={}
title=[[to7(F.A[v]) for v in c] for c in F.compose('RAY TRACING')]
t0,t1=crop_rows(title); out['RAY TRACING']=place('RAY TRACING',title,234,352,t1-t0,5,t0)
ultra=[[to7(F.A[v]) for v in c] for c in F.compose('ULTRA')]
u0,u1=crop_rows(ultra); out['ULTRA']=place('ULTRA',ultra,410,326,u1-u0,5,u0)
helpc=H.compose(['R','a','y',' ','t','r','a','c','e','d',' ','shadows'])
out['help_rt']=place('help_rt',helpc,272,286,20,2,0)
set_indices(buf,r7,ix)
open('../../sodfix/data/option_e.tbn2','wb').write(buf)
json.dump(out,open('work/assets/rt_row_rects.json','w'),indent=1); print(out)
from PIL import Image
im=decode(bytes(buf),r7); bg=Image.new('RGBA',im.size,(40,40,40,255)); bg.alpha_composite(im)
bg.crop((220,220,512,380)).resize((584,320),Image.NEAREST).save('work/assets/png/opt7_rt.png')
