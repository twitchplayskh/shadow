# Builds SoDFix\relief.txt (surface depth materials) from the floor candidate sheets of sod/ssr/sheets2.py.
# Same line format as reflect.txt; the class letter is the material: C cobblestone / sett, P paving slabs, B brick paving.
import pickle, sys
r = pickle.load(open('work/tmp/sod_texuse.pkl', 'rb'))
items = {i: keys for i, keys, pos in pickle.load(open('work/tmp/sheet2_items.pkl', 'rb'))}
# cobbles / setts, the cobble-to-plaster transition pieces (16, 89) and pebble grounds (39, 65)
C = [1, 5, 10, 12, 17, 24, 29, 46, 49, 102, 182, 377, 16, 89, 18, 39, 65]
# paving slabs, flagstones, stone blocks and square stone tiles
P = [72, 101, 122, 153, 353, 391, 413, 38, 143, 177, 225, 168, 207, 354, 420, 553, 554, 361, 479]
# brick paving and stone-brick pathways
B = [94, 103, 108, 115, 35, 44, 76, 73, 75, 227, 257, 259, 261, 550]
out = ['# Lebensbaum surface depth (relief) materials, generated from ccc.wad by tools/relief/make_relief_list.py',
       '# page <table index> <page w> <page h> <x> <y> <w> <h> <class>   region of a texture page',
       '# slot <table index> <class>                                     a sub-texture cut from a page at load',
       '# class: C cobblestone / setts, P stone paving slabs / flagstones, B brick paving']
seen = set()
for cls, ids in (('C', C), ('P', P), ('B', B)):
    for i in ids:
        for k in items[i]:
            if isinstance(k, tuple):
                _, pid, sid = k
                pw, ph, subs = r['pages'][pid][3], r['pages'][pid][4], r['pages'][pid][5]
                for (s, x, y, w, h) in subs:
                    if s == sid:
                        line = 'page %d %d %d %d %d %d %d %s' % (pid, pw, ph, x, y, w, h, cls)
            else:
                line = 'slot %d %s' % (k - 0x10000, cls)
            if line not in seen: seen.add(line); out.append(line)
open(sys.argv[1], 'w').write('\n'.join(out) + '\n')
print(len(out) - 4, 'lines')
