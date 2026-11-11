# Wall textures for IMPROVED GEOMETRY (appended to geometry.txt): every plain outside-wall surface (stone, brick,
# render, plaster) among the images the game uses on vertical faces; windows, doors, timber framing, wood, plants,
# roofs, signs, decorations and images with a painted band or gradient are left to the game. Picked by eye from
# contact sheets of all wall images (wallsheet_all.py; indices below). Per image, measured on the game's own walls:
# tile width / height in world units (median over the polygons where the image stands upright or lies on its side),
# which way up it is used most, and whether it is used turned by 90 degrees.
import pickle, numpy as np
targets, stats = pickle.load(open('/tmp/wall_stats.pkl', 'rb'))
allc = pickle.load(open('/tmp/wall_all.pkl', 'rb'))
PICK = [7,12,16,18,19,21,23,24,31,32,34,35,36,38,43,45,51,54,63,
        64,68,69,71,80,82,85,88,89,96,98,105,112,113,117,124,125,
        128,129,130,132,133,137,138,142,145,149,150,151,152,156,157,160,161,164,165,167,169,170,171,172,175,178,179,180,184,185,186,187,188,189,190,191,
        194,197,198,200,204,213,217,218,219,225,230,231,234,238,241,242,247,250,
        257,258,264,268,273,274,275,279,283,286,288,289,293,296,299,301,302,304,305,312,
        321,324,327,329,330,334,355,356,357,361,362,363,377,383,
        400,404,411,412,413,420,432,
        448,457,472,478,484,485]
lines = ['# walls: plain stone / brick / render / plaster, one aligned layout per wall face (rows of bricks line up);',
         '# the tile size is measured on each wall as it is converted, the size here is the fallback',
         '# wallpage <table index> <page w> <page h> <x> <y> <w> <h> <tile width> <tile height> <v sign: 1 upright> [<turned 90>]',
         '# wallslot <table index> <tile width> <tile height> <v sign> [<turned 90>]']
skipped = []
for i in PICK:
    t = allc[i][1]; a = np.array(stats[t]); pw, ph, x0, y0, w, h, item = targets[t]
    up, side = [], []
    for row in a:
        J = np.array([[row[8], row[9]], [row[10], row[11]]])
        try: Ji = np.linalg.inv(J)          # rows: world (along wall, up) per texel u / per texel v
        except np.linalg.LinAlgError: continue
        lu, lv = np.linalg.norm(Ji[0]), np.linalg.norm(Ji[1])
        if abs(Ji[1][1]) >= 0.9 * lv: up.append((w * lu, h * lv, -np.sign(Ji[1][1])))        # v runs up/down the wall
        elif abs(Ji[0][1]) >= 0.9 * lu: side.append((h * lv, w * lu, -np.sign(Ji[0][1])))    # u runs up/down (turned)
    turned = len(side) > len(up)
    use = np.array(side if turned else up)
    # the renderer measures each wall's own tile size as it converts it; this size is only the fallback
    if len(use) == 0: tw, th, vs = 1000.0, 1000.0, 1; skipped.append((i, t))
    else:
        tw, th = np.median(use[:, 0]), np.median(use[:, 1]); vs = 1 if use[:, 2].mean() >= 0 else -1
        if not (2 < tw < 100000 and 2 < th < 100000): tw, th = 1000.0, 1000.0
    ex = ' 1' if turned else ''
    if t[0] == 'p': lines.append('wallpage %d %d %d %d %d %d %d %.1f %.1f %d%s' % (t[1], pw, ph, x0, y0, w, h, tw, th, vs, ex))
    else: lines.append('wallslot %d %.1f %.1f %d%s' % (t[1], tw, th, vs, ex))
print(len(PICK), 'picked,', len(lines) - 3, 'listed,', sum(1 for l in lines if l.endswith(' 1') and l.startswith('wall')), 'turned')
for s in skipped: print('skipped', s)
open('/tmp/walls.txt', 'w').write('\n'.join(lines) + '\n')
