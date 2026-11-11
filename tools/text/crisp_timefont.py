# CRISP TEXT: the clock digits (timefont.tbn2, texture table index 488) redrawn at 8x for 4K.
# The sheet's digits are thin, stepped 4-bit pixels that cannot be traced cleanly, so each one is drawn again from
# Liberation Serif (SIL Open Font License 1.1) and fitted into the original digit's box (one common width
# stretch for all digits, so the strokes match); the separators are rounded dots on the original dots, AM / PM
# from Liberation Sans. Writes SoDFix\crisp\488.a8 (width, height, coverage bytes for the page at 8x).
# Usage: crisp_timefont.py <timefont.png (extract_sheets.py)> <out.a8> [preview.png]
import numpy as np, sys, struct
from PIL import Image, ImageDraw, ImageFont
K=8
src=np.asarray(Image.open(sys.argv[1]).convert('RGBA'))[...,3].astype(np.float32)/255
SER='/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf'
SANS='/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf'
# (char, x0, y0, x1, y1) in sheet pixels (exclusive ends), from the sheet's own letter boxes
cells=[('0',1,1,35,12),('1',49,1,58,12),('2',75,1,103,12),
       ('3',3,15,32,27),('4',37,15,71,27),('5',75,15,103,27),
       ('6',3,29,33,41),('7',39,29,68,41),('8',75,29,105,41),
       ('9',3,43,32,55),(':',41,43,49,55),('.',57,43,65,55),('/',78,43,99,55),
       ('A',7,57,18,63),('M',21,57,34,63),('P',59,57,66,63),('M',69,57,82,63)]
out=np.zeros((128*K,128*K),np.float32)
def tight(ch, font):
    im=Image.new('L',(800,800)); d=ImageDraw.Draw(im); d.text((100,100),ch,font=font,fill=255)
    a=np.asarray(im); ys,xs=np.nonzero(a>8); return im.crop((xs.min(),ys.min(),xs.max()+1,ys.max()+1))
import statistics
fS=ImageFont.truetype(SER,400); fA=ImageFont.truetype(SANS,400)
def box(x0,y0,x1,y1):
    sub=src[y0:y1,x0:x1]>0.25; ys,xs=np.nonzero(sub); return x0+xs.min(),y0+ys.min(),x0+xs.max()+1,y0+ys.max()+1
facs=[]
for ch,x0,y0,x1,y1 in cells:
    if ch.isdigit() and ch!='1':
        bx0,by0,bx1,by1=box(x0,y0,x1,y1); t=tight(ch,fS); facs.append(((bx1-bx0)/(by1-by0))/(t.width/t.height))
FAC=statistics.median(facs)
for ch,x0,y0,x1,y1 in cells:
    # exact box of the original letter inside the cell
    sub=src[y0:y1,x0:x1]>0.25; ys,xs=np.nonzero(sub)
    bx0,by0,bx1,by1=x0+xs.min(),y0+ys.min(),x0+xs.max()+1,y0+ys.max()+1
    if ch=='1': bx0-=4; bx1+=4          # the sheet's 1 is a bare stem; the redrawn 1 keeps its foot (room either side)
    W,H=(bx1-bx0)*K,(by1-by0)*K
    if ch in ':.':
        g=Image.new('L',(W,H)); d=ImageDraw.Draw(g)
        # the sheet's dots: each run of rows is one dot (drawn squashed; the game stretches it back)
        rows=np.nonzero((src[by0:by1,bx0:bx1]>0.25).any(1))[0]
        runs=[];s=rows[0];p=rows[0]
        for r in rows[1:]:
            if r!=p+1: runs.append((s,p)); s=r
            p=r
        runs.append((s,p))
        for s,e in runs: d.rounded_rectangle((0,s*K,W-1,(e+1)*K-1),radius=min(W,(e+1-s)*K)//3,fill=255)
    else:
        t=tight(ch, fA if ch in 'AMP' else fS)
        if ch.isdigit():
            gw=min(W,int(round(H*t.width/t.height*FAC))); g0=t.resize((gw,H),Image.LANCZOS)
            g=Image.new('L',(W,H)); g.paste(g0,((W-gw)//2,0))
        else: g=t.resize((W,H),Image.LANCZOS)
    out[by0*K:by0*K+H,bx0*K:bx0*K+W]=np.maximum(out[by0*K:by0*K+H,bx0*K:bx0*K+W],np.asarray(g,np.float32)/255)
a8=(np.clip(out,0,1)*255).astype(np.uint8)
open(sys.argv[2],'wb').write(struct.pack('<II',a8.shape[1],a8.shape[0])+a8.tobytes())
if len(sys.argv)>3: Image.fromarray(a8).save(sys.argv[3])
