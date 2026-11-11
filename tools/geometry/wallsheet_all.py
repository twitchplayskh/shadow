import sys, pickle
sys.path.insert(0, '/home/claude/sodmap')
from wad import *
from PIL import Image, ImageDraw
r = pickle.load(open('/home/claude/sod/ssr/sod_texuse.pkl', 'rb'))
rows = pickle.load(open('/tmp/wall_rows.pkl', 'rb'))
pc = {}
def page(pid):
    if pid not in pc:
        ei, cc, sz = r['pages'][pid][:3]; pc[pid] = compose_page(ccc_entry(ei), cc, sz)[0]
    return pc[pid]
def img(t):
    if t[0] == 'p':
        _, pid, x0, y0, w, h = t; return page(pid).crop((x0, y0, x0 + w, y0 + h))
    cut = r['cuts'][t[1]]; p = page(cut['page'])
    x0, y0 = (cut['x'] - 1) // 2, (cut['y'] - 1) // 2; w, h = max((cut['w'] + 3) // 2, 1), max((cut['h'] + 3) // 2, 1)
    return p.crop((x0, y0, x0 + w, y0 + h))
allc = [rw for rw in rows if rw[2] >= 20 and not (rw[3] >= 512)]
st = int(sys.argv[2]); sel = allc[st:st + 64]
C = 8; S = 128
sheet = Image.new('RGB', (C * (S + 8), ((len(sel) + C - 1) // C) * (S + 22)), (30, 30, 30))
dr = ImageDraw.Draw(sheet)
for n, rw in enumerate(sel):
    im = img(rw[1]).convert('RGB')
    # 2x2 repeat to judge tiling
    t = Image.new('RGB', (im.width * 2, im.height * 2))
    for a in range(2):
        for b in range(2): t.paste(im, (a * im.width, b * im.height))
    t = t.resize((S, S))
    x, y = (n % C) * (S + 8), (n // C) * (S + 22)
    sheet.paste(t, (x, y)); dr.text((x + 2, y + S + 2), '%d' % (st + n), fill=(255, 255, 0))
sheet.save(sys.argv[1])
pickle.dump(allc, open('/tmp/wall_all.pkl', 'wb'))
