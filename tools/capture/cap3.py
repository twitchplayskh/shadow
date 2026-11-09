import struct, parse_capture2 as P
def load(path):
    fr,nf,ev=P.load(path)
    d=open(path,'rb').read(); o=20; tids=[]
    while o+8<=len(d):
        tag,ln=struct.unpack_from('<II',d,o); o+=8
        if tag==0x58444954: tids.append(struct.unpack_from('<i',d,o)[0])
        o+=ln
    k=0
    for e in ev:
        if e['tag']=='DRAW': e['tidx']=tids[k]; k+=1
    return fr,nf,ev
