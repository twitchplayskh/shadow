# Offline prototype of the GPU height generator (same math as shaders/sod_relief_gen.comp), wrap addressing.
import numpy as np, sys
from PIL import Image
def sh(a, dx, dy): return np.roll(np.roll(a, -dy, 0), -dx, 1)      # a[y+dy, x+dx]
def box(a, r):
    s = np.zeros_like(a); n = 0
    for dy in range(-r, r+1):
        for dx in range(-r, r+1): s += sh(a, dx, dy); n += 1
    return s / n
def gauss(a, r, sig):
    s = np.zeros_like(a); n = 0
    for dy in range(-r, r+1):
        for dx in range(-r, r+1):
            w = np.exp(-(dx*dx+dy*dy)/(2*sig*sig)); s += w*sh(a, dx, dy); n += w
    return s / n
PAR = {  # cls: gap scale, dome radius, dome power, plateau, detail
    'C': dict(dome=0.55, plateau=0.35, detail=0.10),
    'P': dict(dome=0.20, plateau=0.80, detail=0.06),
    'B': dict(dome=0.25, plateau=0.75, detail=0.06),
}
def gen(img, cls):
    A = np.asarray(img.convert('RGB'), np.float32) / 255.0
    H, W = A.shape[:2]; S = min(W, H)
    L = A @ np.array([0.299, 0.587, 0.114], np.float32)
    # pass 1: features
    Ls = gauss(L, 1, 0.8)
    R = max(2, min(6, S // 10))
    mu = box(Ls, R); var = np.maximum(box(Ls*Ls, R) - mu*mu, 0); sd = np.sqrt(var) + 0.02
    dark = (mu - Ls) / sd                                            # darker than the neighbourhood (z-score)
    def valley(a, st):
        lxx = sh(a, st, 0) + sh(a, -st, 0) - 2*a; lyy = sh(a, 0, st) + sh(a, 0, -st) - 2*a
        lxy = (sh(a, st, st) - sh(a, st, -st) - sh(a, -st, st) + sh(a, -st, -st)) / 4
        return (lxx + lyy) / 2 + np.sqrt(((lxx - lyy) / 2)**2 + lxy**2)   # largest eigenvalue: > 0 across a dark line
    v = np.maximum(valley(Ls, 1), valley(gauss(L, 2, 1.3), 2) * 0.6) / sd
    # chroma: mortar / gaps differ in colour from the stone faces
    Cm = np.stack([box(A[..., c], R) for c in range(3)], -1)
    chroma = np.linalg.norm((A - Cm) - (L - mu)[..., None], axis=-1) / (sd + 0.03)
    score = np.maximum(v, 0) * 0.8 + np.maximum(dark, 0) * 0.45 + 0.1 * np.clip(chroma - 0.8, 0, 2) * (dark > 0)
    score = score * (v > -0.2)
    q = {'C': 0.30, 'P': 0.14, 'B': 0.16}[cls]
    hist, edges = np.histogram(score, 256, (0, 4))
    c = np.cumsum(hist[::-1])[::-1] / score.size           # fraction of texels at or above each bin
    b = np.argmax(c <= q); t = max(edges[b], 0.15)
    gap = np.clip((score - t) / (0.6 * t) + 0.5, 0, 1)
    # joints are lines: keep gap texels with support along one of 4 directions (drops stains and specks)
    Lr = max(2, int(round(S * 0.06)))
    sup = np.zeros_like(gap)
    for (dx, dy) in ((1, 0), (0, 1), (1, 1), (1, -1)):
        acc = np.zeros_like(gap)
        for k in range(-Lr, Lr + 1): acc += sh(gap, dx * k, dy * k)
        sup = np.maximum(sup, acc / (2 * Lr + 1))
    gap = gap * np.clip((sup - 0.35) / 0.25, 0, 1)
    p = PAR[cls]
    solid = 1 - gauss(gap, 1, 0.6)
    rd = max(1, int(round(S * 0.04)))
    d1 = gauss(solid, rd, rd * 0.7); d2 = gauss(solid, 2*rd, rd * 1.4)
    dome = np.clip(0.7*d1 + 0.3*d2, 0, 1)
    plat = np.clip((solid - 0.2) / 0.5, 0, 1)
    h = p['dome'] * dome**1.5 + p['plateau'] * plat * (0.6 + 0.4*dome)
    h = h + p['detail'] * np.clip((Ls - mu) / sd, -2, 2) * 0.5 * plat
    h = (h - h.min()) / max(h.max() - h.min(), 1e-4)
    return h, gap
if __name__ == '__main__':
    import pickle
    ids = {'C': [1,5,10,12,17,24,29,46,49,102,182,377], 'P': [72,101,122,153,353,391,413], 'B': [94,103,108,115]}
    T = 128; rows = []
    for cls, l in ids.items():
        for i in l:
            im = Image.open('work/tmp/sheets2/t%03d.png' % i)
            h, g = gen(im, cls)
            # simple lit preview: normal from height, light from upper left
            s = 2.5 * max(im.size) / 32
            gx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * s; gy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * s
            n = np.stack([-gx, -gy, np.ones_like(h)], -1); n /= np.linalg.norm(n, axis=-1, keepdims=True)
            Ld = np.array([-0.5, -0.5, 0.7]); Ld /= np.linalg.norm(Ld)
            lit = np.clip(n @ Ld, 0, 1) / Ld[2]
            A = np.asarray(im.convert('RGB'), np.float32)
            out = np.clip(A * (0.45 + 0.55 * lit)[..., None], 0, 255)
            tiles = [im.convert('RGB'), Image.fromarray((g*255).astype(np.uint8)).convert('RGB'),
                     Image.fromarray((h*255).astype(np.uint8)).convert('RGB'), Image.fromarray(out.astype(np.uint8))]
            row = Image.new('RGB', (T*4, T)); [row.paste(t.resize((T, T), Image.NEAREST), (k*T, 0)) for k, t in enumerate(tiles)]
            rows.append(row)
    cols = 2; per = (len(rows) + cols - 1) // cols
    sheet = Image.new('RGB', (cols*T*4 + 8, per*T), (30, 30, 30))
    for k, r in enumerate(rows): sheet.paste(r, ((k // per) * (T*4 + 8), (k % per) * T))
    sheet.save('work/tmp/relief_proto.png')
