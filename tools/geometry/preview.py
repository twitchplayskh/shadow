import numpy as np, sys
from PIL import Image
def smooth_noise(n, cells, seed):
    rng = np.random.default_rng(seed); g = rng.random((cells, cells))
    y, x = np.mgrid[0:n, 0:n] * cells / n; x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int); fx = x - x0; fy = y - y0
    sx = fx * fx * (3 - 2 * fx); sy = fy * fy * (3 - 2 * fy)
    g00 = g[y0 % cells, x0 % cells]; g10 = g[y0 % cells, (x0 + 1) % cells]; g01 = g[(y0 + 1) % cells, x0 % cells]; g11 = g[(y0 + 1) % cells, (x0 + 1) % cells]
    return (g00 * (1 - sx) + g10 * sx) * (1 - sy) + (g01 * (1 - sx) + g11 * sx) * sy
def make_mask(n=64, lo=0.47, hi=0.53):
    m1 = 0.65 * smooth_noise(n, 4, 1) + 0.35 * smooth_noise(n, 8, 2)
    m2 = 0.65 * smooth_noise(n, 4, 3) + 0.35 * smooth_noise(n, 8, 4)
    st = lambda v: np.clip((v - lo) / (hi - lo), 0, 1) ** 2 * (3 - 2 * np.clip((v - lo) / (hi - lo), 0, 1))
    return st(m1), st(m2)
LAYERS = [(0.0, False, 0.00, 0.00), (90.0, False, 0.37, 0.61), (180.0, True, 0.71, 0.23)]   # angle, mirror, offset
MASK_SCALE, MASK_ANGLE = 3.3, 17.0
def sample(T, u, v):   # bilinear wrap; u, v in tiles
    h, w = T.shape[:2]; fu = (u * w - 0.5) % w; fv = (v * h - 0.5) % h
    x0 = np.floor(fu).astype(int); y0 = np.floor(fv).astype(int); tx = (fu - x0)[..., None]; ty = (fv - y0)[..., None]
    g = lambda yy, xx: T[yy % h, xx % w]
    return (g(y0, x0) * (1 - tx) + g(y0, x0 + 1) * tx) * (1 - ty) + (g(y0 + 1, x0) * (1 - tx) + g(y0 + 1, x0 + 1) * tx) * ty
def uvs(X, Z, size, ang, mirror, ou, ov):
    c, s = np.cos(np.radians(ang)), np.sin(np.radians(ang))
    u = (X * c + Z * s) / size + ou; v = (-X * s + Z * c) / size + ov
    return (-u if mirror else u), v
def improved(T, X, Z, size, mask):
    mu, mv = uvs(X, Z, size * MASK_SCALE, MASK_ANGLE, False, 0.13, 0.29)
    M1 = sample(mask[0][..., None], mu, mv)[..., 0]; M2 = sample(mask[1][..., None], mu, mv)[..., 0]
    A = sample(T, *uvs(X, Z, size, *LAYERS[0])); B = sample(T, *uvs(X, Z, size, *LAYERS[1])); C = sample(T, *uvs(X, Z, size, *LAYERS[2]))
    r = A * (1 - M1[..., None]) + B * M1[..., None]
    return r * (1 - M2[..., None]) + C * M2[..., None]
def original(T, X, Z, size, seed=5):
    rng = np.random.default_rng(seed); n = 64; rot = rng.integers(0, 4, (n, n)); mir = rng.integers(0, 2, (n, n))
    qx = np.floor(X / size).astype(int); qz = np.floor(Z / size).astype(int)
    fu = X / size - qx; fv = Z / size - qz
    r = rot[qz % n, qx % n]; m = mir[qz % n, qx % n]
    u = np.where(r == 0, fu, np.where(r == 1, fv, np.where(r == 2, 1 - fu, 1 - fv)))
    v = np.where(r == 0, fv, np.where(r == 1, 1 - fu, np.where(r == 2, 1 - fv, fu)))
    u = np.where(m == 1, 1 - u, u)
    return sample(T, u, v)
if __name__ == '__main__':
    T = np.asarray(Image.open('work/tmp/sheets2/t%03d.png' % int(sys.argv[1])).convert('RGB'), np.float32) / 255
    size = 620.0; N = 512; span = size * 7
    Z, X = np.mgrid[0:N, 0:N] * span / N
    mask = make_mask()
    a = original(T, X, Z, size); b = improved(T, X, Z, size, mask)
    im = np.concatenate([a, np.full((N, 8, 3), 0.1), b], 1)
    Image.fromarray((np.clip(im, 0, 1) * 255).astype(np.uint8)).save('work/tmp/geometry_preview.png')
