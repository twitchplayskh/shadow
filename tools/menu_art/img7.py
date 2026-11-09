import sys; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from tbn2 import *
d7=open('work/assets/option_e.tbn2','rb').read()
R7=parse(d7)[7]; W7=R7['w']; H7=R7['h']; P7=palette(d7,R7); A7=[c[3] for c in P7]; I7=indices(d7,R7)
def px7(x,y): return I7[y*W7+x]
def prof(a,b,y0,y1): return [sum(A7[px7(x,y)] for y in range(y0,y1)) for x in range(a,b)]
