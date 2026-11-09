import sys,pickle,os,hashlib; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from wad import *
from PIL import Image, ImageDraw
r=pickle.load(open('sod_texuse.pkl','rb')); u=r['use']
listed=set()
for l in open('../../sodfix/data/reflect.txt'):
    p=l.split()
    if p and p[0]=='slot': listed.add(int(p[1]))
pc={}
def page(pid):
    if pid not in pc:
        if pid not in r['pages']: pc[pid]=None
        else: ei,cc,sz=r['pages'][pid][:3]; pc[pid]=compose_page(ccc_entry(ei),cc,sz)[0]
    return pc[pid]
def img_for(k):
    if isinstance(k,tuple):
        _,pid,sid=k; p=page(pid)
        if p is None: return None
        for (i,sx,sy,sw,sh) in r['pages'][pid][5]:
            if i==sid: return p.crop((sx,sy,sx+sw,sy+sh)),(pid,sx,sy,sw,sh)
        return None
    if k<0x8000: return None
    slot=k-0x10000; cut=r['cuts'].get(slot)
    if not cut or slot in listed: return None
    p=page(cut['page'])
    if p is None: return None
    x0,y0=(cut['x']-1)//2,(cut['y']-1)//2; w,h=max((cut['w']+3)//2,1),max((cut['h']+3)//2,1)
    return p.crop((x0,y0,x0+w,y0+h)),('slot',slot)
c=[]
for k,v in u.items():
    vert=v['area']-v['pos']-v['neg']
    if v['area']<=0 or vert/v['area']<0.6 or vert<2e6: continue
    if isinstance(k,int) and k<0x8000: continue
    c.append((vert,k))
c.sort(key=lambda t:-t[0])
items=[]; seen={}
for vert,k in c:
    res=img_for(k)
    if res is None: continue
    im,ident=res; im=im.convert('RGB')
    if im.size[0]<8 or im.size[1]<8: continue
    h=hashlib.md5(im.tobytes()).hexdigest()
    if h in seen: seen[h][2].append(ident); continue
    rec=[len(items),im,[ident],vert]; seen[h]=rec; items.append(rec)
    if len(items)>=448: break
os.makedirs('work/tmp/wallsheets',exist_ok=True)
for f in os.listdir('work/tmp/wallsheets'): os.remove('work/tmp/wallsheets/'+f)
T=112; N=56; cols=8
for s in range(0,len(items),N):
    chunk=items[s:s+N]; rows=(len(chunk)+cols-1)//cols
    sh=Image.new('RGB',(cols*T,rows*(T+14)),(30,30,30)); dr=ImageDraw.Draw(sh)
    for j,(i,im,ids,vert) in enumerate(chunk):
        x,y=(j%cols)*T,(j//cols)*(T+14)
        sh.paste(im.resize((T-4,T-4)),(x+2,y+2)); dr.text((x+3,y+T-1),'%d'%i,fill=(255,255,0))
    sh.save('work/tmp/wallsheets/sheet_%02d.png'%(s//N))
pickle.dump([(i,ids,vert) for i,im,ids,vert in items],open('work/wall_items.pkl','wb'))
print('items',len(items))
