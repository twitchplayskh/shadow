import sys, json, struct; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from tbn2 import *
import itemfont as F
from img7 import d7, R7, W7, H7, P7, A7, I7, px7
from PIL import Image
import pefile
# --- existing rects on texture 819 (occupied) -----------------------------------
pe=pefile.PE('work/shadow.exe'); img=pe.get_memory_mapped_image(); base=0x400000
used=[]
for i in range(0x5d):
    t,p,u,v,w,h=struct.unpack_from('<HHhhhh',img,0x939450-base+i*12)
    if t==819: used.append((u,v,w,h))
# --- alpha -> image7 index (palette is white with every alpha level) --------------
a2i={}
for i,c in enumerate(P7):
    if c[:3]==(255,255,255) and c[3] not in a2i: a2i[c[3]]=i
def to7(alpha): return a2i[min(a2i,key=lambda a:abs(a-alpha))]
# --- help font glyphs --------------------------------------------------------------
def bottom7(x0,x1,y0,y1): return max(y for y in range(y0,y1) for x in range(x0,x1) if A7[px7(x,y)]>=100)
HL=20
ref_b=bottom7(62,108,73,93)       # baseline of 'screen'
HBASE=ref_b-73
def strip7(x0,x1,y0,y1,base_cols):
    b=bottom7(base_cols[0],base_cols[1],y0,y1); cols=[]
    for x in range(x0,x1):
        col=[0]*HL
        for y in range(HL):
            sy=b-HBASE+y
            if y0<=sy<y1: col[y]=px7(x,sy)
        cols.append(col)
    return cols
R1=(62,108); R2=(122,156)   # descender-free reference: 'screen', 'door'
help_cols = strip7(0,112,73,93,R1) + strip7(167,180,374,398,R2) + strip7(131,140,374,398,R2) + strip7(122,131,374,398,R2) + strip7(160,168,374,398,R2)
# --- item font labels (atlas 8 -> image 7 indices) ----------------------------------
labels={t:[[to7(F.A[v]) for v in col] for col in F.compose(t)] for t in ['SCREEN MODE','WINDOWED','BORDERLESS','FULLSCREEN']}
# --- explicit slots, validated against pixels and existing rects -----------------
def rects_for(tex):
    r=[]
    for i in range(0x5d):
        t,pp,u,v,w,h=struct.unpack_from('<HHhhhh',img,0x939450-base+i*12)
        if t==tex: r.append((u,v,w,h))
    return r
buf=bytearray(open('../../sodfix/data/option_e.tbn2','rb').read())
recs=parse(bytes(buf)); r7=recs[7]; r8=recs[8]
out7=indices(bytes(buf),r7); out8=indices(bytes(buf),r8)
extra_rects={887:[(150,226,111,32),(266,226,108,32),(382,226,104,32),(248,478,156,32)],819:[]}
def check(tex,u,v,w,h):
    W_,H_,ix = (512,400,out7) if tex==819 else (512,512,out8)
    assert u+w<=W_ and v+h<=H_, ('bounds',tex,u,v,w,h)
    assert all(ix[y*W_+x]==0 for y in range(v,v+h) for x in range(u,u+w)), ('pixels',tex,u,v,w,h)
    for (a,b,c,e) in rects_for(tex)+extra_rects[tex]:
        assert u+w<=a or a+c<=u or v+h<=b or b+e<=v, ('rect overlap',tex,(u,v,w,h),(a,b,c,e))
    extra_rects[tex].append((u,v,w,h))
rects={}
def blit(name,cols,tex,u,v,h,pad):
    w=len(cols)+pad*2; check(tex,u,v,w,h)
    ix = out7 if tex==819 else out8
    for i,col in enumerate(cols):
        for y,val in enumerate(col): ix[(v+y)*512+u+pad+i]=val
    rects[name]=dict(tex=tex,u=u,v=v,w=w,h=h)
def crop(cols):
    rows=[y for y in range(len(cols[0])) if any(c[y] for c in cols)]
    t,bm=max(0,rows[0]-1),min(len(cols[0]),rows[-1]+2)
    return [c[t:bm] for c in cols], t, bm-t
assert not any(c[-1] for c in help_cols), 'help last row not empty'
blit('help',[c[:19] for c in help_cols],819,330,93,19,2); rects['help']['yoff']=0
for name,(u,v) in {'SCREEN MODE':(234,326),'WINDOWED':(234,352),'BORDERLESS':(274,234),'FULLSCREEN':(274,262)}.items():
    c,yoff,h=crop(labels[name]); blit(name,c,819,u,v,h,5); rects[name]['yoff']=yoff
set_indices(buf,r7,out7); set_indices(buf,r8,out8)
open('../../sodfix/data/option_e.tbn2','wb').write(buf)
json.dump(rects,open('work/assets/mode_rects.json','w'),indent=1); print(rects)
for k,r in [(7,r7),(8,r8)]:
    im=decode(bytes(buf),r); bg=Image.new('RGBA',im.size,(40,40,40,255)); bg.alpha_composite(im); bg.save('work/assets/png/opt%d_new.png'%k)
