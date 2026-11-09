import sys; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from tbn2 import *
d=open('work/assets/option_e.tbn2','rb').read()
R=parse(d)[8]; W,H=R['w'],R['h']
idx=indices(d,R)
def px(x,y): return idx[y*W+x]
def segs(u,v,w,h):
    cols=[any(px(x,y) for y in range(v,v+h)) for x in range(u,u+w)]
    out=[];x=0
    while x<w:
        if cols[x]:
            s=x
            while x<w and cols[x]: x+=1
            out.append((u+s,u+x))   # [x0,x1)
        else: x+=1
    return out
def rowspan(x0,x1,v,h):
    ys=[y for y in range(v,v+h) if any(px(x,y) for x in range(x0,x1))]
    return (min(ys),max(ys)+1) if ys else None
if __name__=='__main__':
    for name,(u,v,w,h) in {'640x480':(170,327,108,32),'800x600':(286,328,108,32),'1024x768':(406,328,106,32),'1280x960':(364,407,128,32)}.items():
        print(name,[(a,b,rowspan(a,b,v,h)) for a,b in segs(u,v,w,h)])
PAL=palette(d,R); A=[c[3] for c in PAL]
def segsA(u,v,w,h,th=60):
    cols=[max(A[px(x,y)] for y in range(v,v+h)) for x in range(u,u+w)]
    out=[];x=0
    while x<w:
        if cols[x]>=th:
            s=x
            while x<w and cols[x]>=th: x+=1
            out.append((u+s,u+x))
        else: x+=1
    return out,cols
