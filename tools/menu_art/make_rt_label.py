import sys, json; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from labels import *
from PIL import Image
# approx glyph spans and the band (y0,y1) they come from
SRC = {'R':(20,36,365,395),'A':(125,141,443,473),'Y':(50,66,126,156),'T':(153,168,443,473),
       'C':(110,124,443,473),'E':(362,373,443,473),'D':(0,19,365,395)}
def colsum(x,y0,y1): return sum(A[px(x,y)] for y in range(y0,y1))
def snap(b,y0,y1): return min(range(b-3,b+4), key=lambda x: colsum(x,y0,y1))
def bottom(x0,x1,y0,y1): return max(y for y in range(y0,y1) for x in range(x0,x1) if A[px(x,y)]>=100)
LH=32; BASEOFF=25     # baseline row inside label rect (MULTIPLE rect v=444, band bottom ~468)
ref_bottom = bottom(366,379,443,473)  # 'E' of MULTIPLE
BASEOFF = ref_bottom-444
def glyph(ch):
    x0,x1,y0,y1=SRC[ch]; 
    b=bottom(x0,x1,y0,y1); cols=[]
    for x in range(x0,x1+1):
        col=[0]*LH
        for y in range(LH):
            sy=b-BASEOFF+y
            if 0<=sy<512 and y0<=sy<y1: col[y]=px(x,sy)
        cols.append(col)
    return cols
def overlay(a,b,ov=1):  # merge last ov columns of a with first of b (keep higher alpha)
    for i in range(ov):
        ca=a[-ov+i]; cb=b[i]
        a[-ov+i]=[p if A[p]>=A[q] else q for p,q in zip(ca,cb)]
    return a+b[ov:]
def compose(text):
    out=[]
    for ch in text:
        if ch==' ': out += [[0]*LH for _ in range(5)]; continue
        g=glyph(ch); out = overlay(out,g) if out and any(out[-1]) else out+g
    return out
if __name__=='__main__':
    cols=compose('RAY TRACED'); print('width',len(cols),'baseoff',BASEOFF)
    json.dump(cols,open('work/assets/rt_cols.json','w'))
    pal=PAL; im=Image.new('RGBA',(len(cols),LH))
    im.putdata([pal[cols[x][y]] for y in range(LH) for x in range(len(cols))])
    ref=decode(d,R).crop((264,444,420,476))
    z=Image.new('RGBA',(420,80),(0,0,0,255)); z.alpha_composite(ref,(0,0)); z.alpha_composite(im,(0,40))
    z.resize((1260,240),Image.NEAREST).save('work/assets/png/rt_label_zoom.png')

def condensed(target_w):
    cols=compose('RAY TRACED')
    a=Image.new('L',(len(cols),LH)); a.putdata([A[cols[x][y]] for y in range(LH) for x in range(len(cols))])
    a=a.resize((target_w,LH),Image.LANCZOS)
    levels=sorted(set((A[i],i) for i in range(16) if PAL[i][0]>=240 or A[i]==0))
    def q(v): return min(levels,key=lambda t:abs(t[0]-v))[1]
    return [[q(a.getpixel((x,y))) for y in range(LH)] for x in range(target_w)]
