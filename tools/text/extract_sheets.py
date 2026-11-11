# Decodes the game's lettering sheets to PNG (RGBA, page layout as the game loads it) for the CRISP TEXT scripts:
#   fonte_all.png (subtitle font, table 77), timefont.png (clock, 488), placename.png (978), ccs_font.png (833)
# Usage: extract_sheets.py <game folder> <out folder>
import sys, os, glob
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'reflections'))
import wad

game, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
for name in ('fonte_all', 'fontu_all', 'timefont', 'placename', 'ccs_font'):
    f = glob.glob(os.path.join(game, 'works', 'conv', '*', name + '.tbn2'))
    if not f:
        print('not found:', name); continue
    x = bytes(16) + open(f[0], 'rb').read()
    page, _ = wad.compose_page(x, 0, len(x))
    page.save(os.path.join(out, name + '.png'))
    print(name, page.size)
