# 0.10: rename RAY TRACING -> RT SHADOW CASTING, add ANTI-ALIASING row art (title, SMAA value, help line)
# into option_e.tbn2 image 7 (tex 819 / pal 13226). Titles and values use the game's own item-font glyphs;
# the help line is set in Liberation Serif at the help font's size, quantised to image 7's palette.
import sys, json; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from tbn2 import *
import itemfont as F
from PIL import Image, ImageDraw, ImageFont
F.G['G']=(193,210,443,473)
SRC='work/assets/option_e_v096_backup.tbn2'      # atlas as shipped up to 0.9.6
buf=bytearray(open(SRC,'rb').read())
recs=parse(bytes(buf)); r7=recs[7]; ix=indices(bytes(buf),r7); W=512
pal7=palette(bytes(buf),r7)
a2i={}
for i,c in enumerate(pal7):
    if c[:3]==(255,255,255) and c[3] not in a2i: a2i[c[3]]=i
to7=lambda a: a2i[min(a2i,key=lambda q:abs(q-a))]
old=json.load(open('work/assets/rt_row_rects.json'))['RAY TRACING']
for y in range(old['v'],old['v']+old['h']):
    for x in range(old['u'],old['u']+old['w']): ix[y*W+x]=0
def place(name,cols,u,v,h,pad,yoff):
    wdt=len(cols)+2*pad
    assert u+wdt<=W, (name,'too wide',u+wdt)
    for y in range(v,v+h):
        for x in range(u,u+wdt): assert ix[y*W+x]==0, (name,'foreign ink',x,y)
    for i,col in enumerate(cols):
        for y,val in enumerate(col[yoff:yoff+h]):
            if val: ix[(v+y)*W+u+pad+i]=val
    return dict(tex=819,u=u,v=v,w=wdt,h=h,yoff=yoff)
def crop_rows(cols):
    rows=[y for y in range(len(cols[0])) if any(c[y] for c in cols)]
    return max(0,rows[0]-1), min(len(cols[0]),rows[-1]+2)
def item(text):
    return [[to7(F.A[v]) for v in c] for c in F.compose(text)]
def hyphen():
    h=F.glyph('H'); mid=len(h)//2
    bar=[y for y in range(F.LH) if F.A[h[mid][y]]>=100]
    w=9; col=[0]*F.LH
    for y in bar: col[y]=h[mid][y]
    return [[0]*F.LH]+[list(col) for _ in range(w)]+[[0]*F.LH]
out={}
t=item('RT SHADOW CASTING'); a,b=crop_rows(t); out['RT SHADOW CASTING']=place('RT SHADOW CASTING',t,234,352,b-a,5,a)
anti=F.compose('ANTI'); ali=F.compose('ALIASING')
aa=[[to7(F.A[v]) for v in c] for c in anti+hyphen()+ali]
a,b=crop_rows(aa); out['ANTI-ALIASING']=place('ANTI-ALIASING',aa,4,113,b-a,5,a)
s=item('SMAA'); a,b=crop_rows(s); out['SMAA']=place('SMAA',s,300,376,b-a,5,a)
# help line, 20 px, baseline like the game's help font (cap height 13 px: rows 0..12 of a 20 px line)
font=ImageFont.truetype('/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf',19)
text='Smooth jagged edges'
tw=int(font.getlength(text))+2
im=Image.new('L',(tw,20),0); d=ImageDraw.Draw(im)
asc=font.getbbox('C')[1]
d.text((1,-asc+1),text,fill=255,font=font)
cols=[[to7(im.getpixel((x,y))*max(a2i)//255) if im.getpixel((x,y))>20 else 0 for y in range(20)] for x in range(tw)]
out['help_aa']=place('help_aa',cols,4,168,20,2,0)
set_indices(buf,r7,ix)
open('../../sodfix/data/option_e.tbn2','wb').write(buf)
json.dump(out,open('work/assets/rt_aa_rects.json','w'),indent=1); print(json.dumps(out))
im=decode(bytes(buf),r7); bg=Image.new('RGBA',im.size,(40,40,40,255)); bg.alpha_composite(im); bg.save('work/tmp/opt7_aa.png')
