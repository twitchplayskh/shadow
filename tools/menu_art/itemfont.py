# Compose labels from Shadow of Destiny's item font glyphs in option_e.tbn2 image 8
import sys; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from labels import d, R, W, PAL, A, px
from PIL import Image
LH=32
SH=(443,473)
G={'S':(1,11)+SH,'H':(12,30)+SH,'A':(31,45)+SH,'D':(46,64)+SH,'O':(64,82)+SH,'W':(82,106)+SH,
   'C':(110,124)+SH,'T':(153,168)+SH,'I':(169,175)+SH,'N':(175,193)+SH,
   'M':(264,285)+SH,'U':(287,304)+SH,'L':(349,361)+SH,'P':(336,349)+SH,'E':(361,374)+SH,
   'R':(20,36,365,395),'F':(12,22,403,435),'B':(95,109,403,435),'Y':(50,66,126,156)}
def bottom(x0,x1,y0,y1): return max(y for y in range(y0,y1) for x in range(x0,x1) if A[px(x,y)]>=100)
BASEOFF=bottom(361,374,*SH)-444     # baseline row inside a 32px label (matches MULTIPLE)
def glyph(ch):
    x0,x1,y0,y1=G[ch]; b=bottom(x0,x1,y0,y1); cols=[]
    for x in range(x0,x1+1):
        col=[0]*LH
        for y in range(LH):
            sy=b-BASEOFF+y
            if y0<=sy<y1: col[y]=px(x,sy)
        cols.append(col)
    return cols
def merge(a,b):
    ca,cb=a[-1],b[0]; a[-1]=[p if A[p]>=A[q] else q for p,q in zip(ca,cb)]; return a+b[1:]
def compose(text,space=6):
    out=[]
    for ch in text:
        if ch==' ': out+= [[0]*LH for _ in range(space)]; continue
        g=glyph(ch); out = merge(out,g) if out and any(out[-1]) else out+g
    return out
def condense(cols,tw):
    a=Image.new('L',(len(cols),LH)); a.putdata([A[cols[x][y]] for y in range(LH) for x in range(len(cols))])
    a=a.resize((tw,LH),Image.LANCZOS)
    lv=sorted(set((A[i],i) for i in range(16) if PAL[i][0]>=240 or A[i]==0))
    q=lambda v: min(lv,key=lambda t:abs(t[0]-v))[1]
    return [[q(a.getpixel((x,y))) for y in range(LH)] for x in range(tw)]
def to_image(cols):
    im=Image.new('RGBA',(len(cols),LH)); im.putdata([PAL[cols[x][y]] for y in range(LH) for x in range(len(cols))]); return im
