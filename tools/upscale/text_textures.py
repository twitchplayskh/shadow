# Writes SoDFix\text_textures.txt: texture table indices of the game's images that hold text (fonts, menus, help,
# save screens, credits, ending texts, place names...). The texture upscaler enlarges these plainly (bilinear).
# Labelled by eye from contact sheets of works/conv/*.tbn2 (two container layouts: menu images by image id,
# texture-chunk files by page id + entry ids). Usage: text_textures.py <game folder> <out>
import sys, os, glob, struct
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, '..', 'reflections'))
import wad
MENU_TEXT = [1227, 1244, 694, 855, 1793, 1810, 1811, 1812, 1813, 1814, 1792, 354, 742, 899, 1051, 1082, 1083, 819, 887,
             4605, 4604, 4603, 4602, 4601, 4600, 1701, 1700, 1699, 1698, 1697, 1696, 1695, 1694, 1693, 1689, 1683, 1688,
             1686, 1685, 1684, 654, 1397, 667, 582, 4606, 472, 1180, 4999]
CHUNK_TEXT = ['fonte_all', 'fontu_all', '2141', '2256', 'book', 'ccs_font', 'coming', 'ending_fd', 'ending_fe', 'ending_ff',
              'ending_fi', 'ending_fs', 'ending_pc', 'nowloading', 'op_staff_roll', 'pause0', 'placename', 'timefont',
              'vmapt', 'vmapw']
game, out = sys.argv[1], sys.argv[2]
lines = ['# Lebensbaum: game textures that hold text (texture table indices). They are enlarged plainly (bilinear) instead',
         '# of through the upscaler and the super-resolution network, which distort lettering.',
         '# menus, help, save, credits, pause, title (image ids)', ' '.join(map(str, MENU_TEXT))]
for name in CHUNK_TEXT:
    f = glob.glob(os.path.join(game, 'works', 'conv', '*', name + '.tbn2'))
    if not f: continue
    x = bytes(16) + open(f[0], 'rb').read()
    pid = struct.unpack_from('<I', x, 0x20)[0]
    ents, _ = wad.tex_entries(x, 0, len(x))
    lines += ['# ' + name + '.tbn2', ' '.join(str(i) for i in [pid] + [e['id'] for e in ents])]
open(out, 'w').write('\n'.join(lines) + '\n')
