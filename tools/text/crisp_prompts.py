# CRISP TEXT button prompts: badges for the menu prompts that show the last-used controller instead of the keyboard
# keys (confirm / back). A filled circle with the button's letter or symbol cut out, so it takes the colour of the
# text it replaces: A, B (Xbox, Steam Deck and other XInput pads; Nintendo pads use the same letters), cross, circle
# (PlayStation). Writes SoDFix\crisp\prompts.a8: u32 width, u32 height, coverage bytes; 4 badges of 128 x 128 in a row.
# Letters: Inter Bold (SIL Open Font License 1.1). Usage: crisp_prompts.py <out.a8> [preview.png]
import sys, struct, numpy as np
from PIL import Image, ImageDraw, ImageFont
S = 128; SS = 8                     # drawn at 8x and averaged down for smooth edges
FONT = '/usr/share/fonts/opentype/inter/Inter-Bold.otf'
def badge(kind):
    N = S * SS; im = Image.new('L', (N, N), 0); d = ImageDraw.Draw(im)
    r = N * 0.47; c = N / 2
    d.ellipse((c - r, c - r, c + r, c + r), fill=255)
    if kind in ('A', 'B'):
        f = ImageFont.truetype(FONT, int(N * 0.62))
        bb = d.textbbox((0, 0), kind, font=f)
        d.text((c - (bb[0] + bb[2]) / 2, c - (bb[1] + bb[3]) / 2), kind, font=f, fill=0)
    elif kind == 'cross':
        w = N * 0.085; e = N * 0.22
        for sx in (1, -1):
            d.line((c - e, c - e * sx, c + e, c + e * sx), fill=0, width=int(w * 2))
    elif kind == 'circle':
        ro = N * 0.25; ri = N * 0.165
        d.ellipse((c - ro, c - ro, c + ro, c + ro), fill=0); d.ellipse((c - ri, c - ri, c + ri, c + ri), fill=255)
    return im.resize((S, S), Image.LANCZOS)
icons = [badge(k) for k in ('A', 'B', 'cross', 'circle')]
atlas = Image.new('L', (S * len(icons), S)); [atlas.paste(ic, (i * S, 0)) for i, ic in enumerate(icons)]
a = np.asarray(atlas, np.uint8)
open(sys.argv[1], 'wb').write(struct.pack('<II', a.shape[1], a.shape[0]) + a.tobytes())
if len(sys.argv) > 2:
    bg = Image.new('RGB', atlas.size, (50, 70, 110)); bg.paste((255, 255, 255), mask=atlas); bg.save(sys.argv[2])
