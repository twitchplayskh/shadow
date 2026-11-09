import sys,pickle,struct,os; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from wad import *
from PIL import Image, ImageDraw
r=pickle.load(open('work/tmp/sod_texuse.pkl','rb')); big=pickle.load(open('work/tmp/floorcands.pkl','rb'))
pagecache={}
def page(pid):
    if pid not in pagecache:
        if pid not in r['pages']: pagecache[pid]=None
        else:
            ei,c,sz=r['pages'][pid]; x=ccc_entry(ei); pagecache[pid]=compose_page(x,c,sz)[0]
    return pagecache[pid]
def crop_for(ref,u):
    if ref<0:
        k=r['cuts'].get(ref)
        if not k: return None
        p=page(k['page'])
        if p is None: return None
        x0,y0=(k['x']-1)//2,(k['y']-1)//2; w,h=max((k['w']+3)//2,1),max((k['h']+3)//2,1)
        return p.crop((x0,y0,x0+w,y0+h))
    p=page(ref)
    if p is None or not u.get('upuv'): return None
    a,u0,v0,u1,v1=max(u['upuv'])
    W,H=p.size
    box=[max(0,int(u0*W)),max(0,int(v0*H)),min(W,int(u1*W)+1),min(H,int(v1*H)+1)]
    if box[2]-box[0]<4 or box[3]-box[1]<4: return None
    return p.crop(box)
os.makedirs('work/tmp/sheets',exist_ok=True)
items=[]
for i,(pos,frac,ref,u) in enumerate(big):
    im=crop_for(ref,u)
    if im is not None: items.append((i,ref,im.convert('RGB')))
T=112; N=48; cols=8
for s in range(0,len(items),N):
    chunk=items[s:s+N]; rows=(len(chunk)+cols-1)//cols
    sh=Image.new('RGB',(cols*T,rows*(T+14)),(30,30,30)); dr=ImageDraw.Draw(sh)
    for j,(i,ref,im) in enumerate(chunk):
        x,y=(j%cols)*T,(j//cols)*(T+14)
        sh.paste(im.resize((T-4,T-4)),(x+2,y+2)); dr.text((x+3,y+T-1),'%d'%i,fill=(255,255,0))
    sh.save('work/tmp/sheets/sheet_%02d.png'%(s//N))
pickle.dump([(i,ref) for i,ref,_ in items],open('work/tmp/sheet_items.pkl','wb'))
print('items',len(items),'sheets',(len(items)+N-1)//N)
