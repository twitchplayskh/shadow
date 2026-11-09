import sys,pickle,struct,os,hashlib; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from wad import *
from PIL import Image, ImageDraw
r=pickle.load(open('work/tmp/sod_texuse.pkl','rb')); c=pickle.load(open('work/tmp/floorcands2.pkl','rb'))
pagecache={}
def page(pid):
    if pid not in pagecache:
        if pid not in r['pages']: pagecache[pid]=None
        else:
            ei,cc,sz=r['pages'][pid][:3]; x=ccc_entry(ei); pagecache[pid]=compose_page(x,cc,sz)[0]
    return pagecache[pid]
def img_for(k):
    if isinstance(k,tuple):
        _,pid,sid=k; p=page(pid)
        if p is None: return None
        for (i,sx,sy,sw,sh) in r['pages'][pid][5]:
            if i==sid: return p.crop((sx,sy,sx+sw,sy+sh))
        return None
    slot=k-0x10000; cut=r['cuts'].get(slot)
    if not cut: return None
    p=page(cut['page'])
    if p is None: return None
    x0,y0=(cut['x']-1)//2,(cut['y']-1)//2; w,h=max((cut['w']+3)//2,1),max((cut['h']+3)//2,1)
    return p.crop((x0,y0,x0+w,y0+h))
items=[]; seen={}
for pos,k,u in c:
    im=img_for(k)
    if im is None: continue
    im=im.convert('RGB'); h=hashlib.md5(im.tobytes()).hexdigest()
    if h in seen: seen[h][2].append(k); continue
    rec=[len(items),im,[k],pos]; seen[h]=rec; items.append(rec)
os.makedirs('work/tmp/sheets2',exist_ok=True)
for f in os.listdir('work/tmp/sheets2'): os.remove('work/tmp/sheets2/'+f)
T=112; N=56; cols=8
for s in range(0,len(items),N):
    chunk=items[s:s+N]; rows=(len(chunk)+cols-1)//cols
    sh=Image.new('RGB',(cols*T,rows*(T+14)),(30,30,30)); dr=ImageDraw.Draw(sh)
    for j,(i,im,keys,pos) in enumerate(chunk):
        x,y=(j%cols)*T,(j//cols)*(T+14)
        sh.paste(im.resize((T-4,T-4)),(x+2,y+2)); dr.text((x+3,y+T-1),'%d'%i,fill=(255,255,0))
    sh.save('work/tmp/sheets2/sheet_%02d.png'%(s//N))
pickle.dump([(i,keys,pos) for i,im,keys,pos in items],open('work/tmp/sheet2_items.pkl','wb'))
for i,im,keys,pos in items: im.save('work/tmp/sheets2/t%03d.png'%i)
print('unique',len(items),'sheets',(len(items)+N-1)//N)
