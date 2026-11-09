import sys; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from sodlib import *
dis(int(sys.argv[1],16),int(sys.argv[2]) if len(sys.argv)>2 else 60)
