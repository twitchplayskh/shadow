import sys; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from img7 import *
from PIL import Image
HL=20
def bottom7(x0,x1,y0,y1): return max(y for y in range(y0,y1) for x in range(x0,x1) if A7[px7(x,y)]>=100)
HBASE=bottom7(62,108,73,93)-73      # baseline of 'screen' in its 20px line
ROWS={'r1':((280,304),(57,66)),'r2':((376,398),(47,54)),'r3':((376,398),(161,167)),'sh':((73,93),(392,453)),'fx':((306,326),(156,163))}
G={'R':('r1',46,57),'a':('r3',197,204),'y':('r3',265,272),'t':('r1',94,100),'r':('r3',150,156),
   'c':('fx',180,188),'e':('r2',47,54),'d':('r2',123,131),'shadows':('sh',392,453)}
def strip(row,x0,x1):
    (y0,y1),(bx0,bx1)=ROWS[row]; b=bottom7(bx0,bx1,y0,y1); cols=[]
    for x in range(x0,x1):
        col=[0]*HL
        for y in range(HL):
            sy=b-HBASE+y
            if y0<=sy<y1: col[y]=px7(x,sy)
        cols.append(col)
    return cols
def compose(tokens,space=5):
    out=[]
    for t in tokens:
        if t==' ': out+=[[0]*HL for _ in range(space)]; continue
        out+=strip(*G[t])
    return out
def to_image(cols):
    im=Image.new('RGBA',(len(cols),HL)); im.putdata([P7[cols[x][y]] for y in range(HL) for x in range(len(cols))]); return im
