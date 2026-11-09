import sys, json; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from labels import *
from PIL import Image
# glyph strips: (x0,x1,label_top_v) from original labels
G = {
 '6':(176,185,327),'4':(437,450,328),'0':(395,406,407),'8':(385,395,407),
 '1':(413,418,328),'2':(429,437,328),'9':(431,441,407),'x':(215,227,327),
}
LH=32
# baseline per source label = bottom of a plain '0' in that label
def bottom(x0,x1,v):
    return max(y for y in range(v,v+LH) for x in range(x0,x1) if A[px(x,y)]>=60)
BASE={327:bottom(257,268,327)-327, 328:bottom(418,429,328)-328, 407:bottom(451,462,407)-407}
print('baseline offsets',BASE)
REF=BASE[327]
def compose(text):
    cols=[]   # list of column arrays (LH indices)
    for ch in text:
        if ch==' ': cols += [[0]*LH for _ in range(6)]; continue
        x0,x1,v=G[ch]; dy=BASE[v]-REF
        for x in range(x0,x1):
            col=[0]*LH
            for y in range(LH):
                sy=v+y+dy
                if v<=sy<v+LH: col[y]=px(x,sy)
            cols.append(col)
    return cols
def region_empty(u,v,w,h): return all(px(x,y)==0 for x in range(u,u+w) for y in range(v,v+h))
new=[('1440 x 1080',None),('1920 x 1440',None),('2880 x 2160',None)]
slots=[(150,226),(266,226),(382,226)]
buf=bytearray(d); out=list(idx); rects=[]
for (text,_),(u,v) in zip(new,slots):
    cols=compose(text); w=len(cols)+8
    assert u+w<=512 and region_empty(u,v,w,LH), (text,u,v,w)
    for i,col in enumerate(cols):
        for y,val in enumerate(col): out[(v+y)*W+u+6+i]=val
    rects.append(dict(text=text,u=u,v=v,w=w,h=LH)); print(text,u,v,w)
set_indices(buf,R,out)
open('../../sodfix/data/option_e.tbn2','wb').write(buf)
json.dump(rects,open('work/assets/new_rects.json','w'))
im=decode(bytes(buf),R); bg=Image.new('RGBA',im.size,(40,40,40,255)); bg.alpha_composite(im); bg.save('work/assets/png/atlas_new.png')
z=Image.new('RGBA',(512,40*7),(0,0,0,255))
allr=[(170,327,108),(286,328,108),(406,328,106),(364,407,128)]+[(r['u'],r['v'],r['w']) for r in rects]
for i,(u,v,w) in enumerate(allr): z.alpha_composite(im.crop((u,v,u+w,v+LH)),(4,i*40))
z.resize((1024,560),Image.NEAREST).save('work/assets/png/new_labels_zoom.png')
# RAY TRACED label (condensed to MULTIPLE's width) at rect (248,478,156,32), glyphs at u+2
rt=json.load(open('work/assets/rt_cols.json'))
assert region_empty(248,478,156,LH)
for i,col in enumerate(rt):
    for y,val in enumerate(col): out[(478+y)*W+250+i]=val
set_indices(buf,R,out)
open('../../sodfix/data/option_e.tbn2','wb').write(buf)
rects.append(dict(text='RAY TRACED',u=248,v=478,w=156,h=LH)); print(rects)
im=decode(bytes(buf),R); bg=Image.new('RGBA',im.size,(40,40,40,255)); bg.alpha_composite(im); bg.save('work/assets/png/atlas_new.png')
