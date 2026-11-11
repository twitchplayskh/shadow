# CRISP TEXT: the subtitle font sheet (fonte_all.tbn2, texture table index 77) redrawn at 8x for 4K.
# The sheet is a 12 x 14 pixel grid; each letter sits at the left of its cell and the game draws it with its own
# width table. Every letter of the main set (A-Z, a-z, 0-9, punctuation) is drawn from Inter (SIL Open Font
# License 1.1) at one size, standing on the original baseline at the original left edge, narrowed only if it
# would be wider than the original letter. Accented letters and the button symbols are traced from the sheet
# (white core, outline dropped), thinned to match. Writes SoDFix\crisp\77.a8 (width, height, coverage bytes).
# Usage: crisp_subtitle_font.py <fonte_all.png (decoded page, RGBA)> <out.a8> [weight] [preview.png]
import sys, struct, numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.ndimage import map_coordinates, gaussian_filter
K = 8
src = np.asarray(Image.open(sys.argv[1]).convert('RGBA')).astype(np.float32) / 255
weight = sys.argv[3] if len(sys.argv) > 3 else 'Medium'
FONT = '/usr/share/fonts/opentype/inter/Inter-%s.otf' % weight
lum = 0.299 * src[..., 0] + 0.587 * src[..., 1] + 0.114 * src[..., 2]
t = np.clip((lum - 0.30) / 0.45, 0, 1); core = src[..., 3] * t * t * (3 - 2 * t)
PH, PW = core.shape
out = np.zeros((PH * K, PW * K), np.float32)

def trace(y0, y1, x0, x1, th=0.52):
    c = gaussian_filter(core, 0.5, mode='nearest', truncate=2.5)
    ys = (np.arange(y0 * K, y1 * K) + 0.5) / K - 0.5; xs = (np.arange(x0 * K, x1 * K) + 0.5) / K - 0.5
    Y, X = np.meshgrid(ys, xs, indexing='ij')
    v = map_coordinates(c, [Y, X], order=3, mode='nearest')
    gy, gx = np.gradient(v); g = np.sqrt(gx * gx + gy * gy) + 1e-4
    return np.clip((v - th) / g + 0.5, 0, 1)

ROWS = ['ABCDEFGHIJ', 'KLMNOPQRST', 'UVWXYZ!?.,', '0123456789', "-'abcdefgh", 'ijklmnopqr', 'stuvwxyz“”', '/:;####&…']
TOP = 128
def cellbox(r, c):
    x0, y0 = c * 12, TOP + r * 14
    sub = core[y0:y0 + 14, x0:x0 + 12] > 0.5
    ys, xs = np.nonzero(sub)
    if len(xs) == 0: return None
    return x0 + xs.min(), y0 + ys.min(), x0 + xs.max() + 1, y0 + ys.max() + 1
# reference sizes from the sheet: cap height (H), x-height (x), baseline per row
H = cellbox(0, 7); capH = H[3] - H[1]
xb = cellbox(6, 5); xH = xb[3] - xb[1]
base = {}
for r in range(8):
    bots = [cellbox(r, c)[3] for c in range(len(ROWS[r])) if cellbox(r, c) and ROWS[r][c] not in "gjpqy,;'“”-" ]
    base[r] = int(np.median(bots)) if bots else None
font = ImageFont.truetype(FONT, 1000)
def ink(ch):
    im = Image.new('L', (1400, 1400)); d = ImageDraw.Draw(im); d.text((200, 100), ch, font=font, fill=255)
    a = np.asarray(im); ys, xs = np.nonzero(a > 4); return im, xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
_, _, hy0, _, hy1 = ink('H'); fcap = hy1 - hy0
_, _, by0, _, by1 = ink('x'); fbase = by1                      # baseline of the reference render
_, _, _, _, xx1 = ink('x'); _, _, xy0, _, _ = ink('x'); fx = xx1 - xy0
# one size for all: the sheet's cap height (in 8x texels); lowercase follow the font's own proportions
S = capH * K / fcap
# one horizontal narrowing for all, from the capitals and lowercase widths, then per letter only if still too wide
ratios = []
for r in (0, 1, 4, 5, 6):
    for c, ch in enumerate(ROWS[r]):
        b = cellbox(r, c)
        if not b or not ch.isalpha(): continue
        _, x0, _, x1, _ = ink(ch); ratios.append(((b[2] - b[0]) * K) / ((x1 - x0) * S))
HS = min(1.0, float(np.median(ratios)) * 1.0)
for r, row in enumerate(ROWS):
    for c, ch in enumerate(row):
        b = cellbox(r, c)
        if not b: continue
        if ch == '#':
            out[b[1] * K:b[3] * K, b[0] * K:b[2] * K] = np.maximum(out[b[1] * K:b[3] * K, b[0] * K:b[2] * K], trace(b[1], b[3], b[0], b[2]))
            continue
        im, x0, y0, x1, y1 = ink(ch)
        g = im.crop((x0, y0, x1, y1))
        gw = max(1, int(round((x1 - x0) * S * HS))); gh = max(1, int(round((y1 - y0) * S)))
        maxw = (b[2] - b[0]) * K + K                                  # never wider than the original letter (+1 px)
        if gw > maxw: gw = maxw
        g = np.asarray(g.resize((gw, gh), Image.LANCZOS), np.float32) / 255
        # baseline: the font's baseline maps to the row's baseline in the sheet
        top = int(round(base[r] * K - (fbase - y0) * S))
        left = b[0] * K
        # inside its own cell only (the cells touch: a tail running into the next row would show on that letter);
        # a descender deeper than the cell allows is drawn shorter, the rest of the letter unchanged
        cy0, cy1 = (TOP + r * 14) * K, (TOP + r * 14 + 14) * K - 1
        if top + gh > cy1 and (fbase - y0) * S < gh:
            above = int(round((fbase - y0) * S)); below = gh - above; room = cy1 - (top + above)
            if room > 0 and below > room:
                full = np.asarray(Image.fromarray((g * 255).astype(np.uint8)).resize((gw, gh)), np.float32) / 255
                lower = Image.fromarray((full[above:] * 255).astype(np.uint8)).resize((gw, room), Image.LANCZOS)
                g = np.concatenate([full[:above], np.asarray(lower, np.float32) / 255]); gh = g.shape[0]
        y_0, x_0 = max(top, cy0), max(left, 0)
        y_1, x_1 = min(top + gh, cy1), min(left + gw, out.shape[1])
        out[y_0:y_1, x_0:x_1] = np.maximum(out[y_0:y_1, x_0:x_1], g[y_0 - top:y_1 - top, x_0 - left:x_1 - left])
# accented letters (top page): traced, thinned
out[0:TOP * K, :] = np.maximum(out[0:TOP * K, :], trace(0, TOP, 0, PW))
a8 = (np.clip(out, 0, 1) * 255).astype(np.uint8)
open(sys.argv[2], 'wb').write(struct.pack('<II', a8.shape[1], a8.shape[0]) + a8.tobytes())
print('cap', capH, 'x', xH, 'HS %.2f' % HS)
if len(sys.argv) > 4:
    Image.fromarray(a8).save(sys.argv[4])
