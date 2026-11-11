# Writes SoDFix\sr_weights.bin for the texture super-resolution (DXVK fork, d3d9_sod_sr.cpp) from Real-ESRGAN's
# realesr-general-x4v3.pth and realesr-general-wdn-x4v3.pth (BSD-3-Clause, https://github.com/xinntao/Real-ESRGAN),
# blended 50/50 (the denoising variant keeps more of the original surface on these small, dithered textures).
# Format: 'LBSR' u32 version=1, u32 layers, then per layer u32 cin, cout, prelu; then all layers' data as float16:
# weights [cout][cin][3][3], bias [cout], prelu slopes [cout] (if prelu).
import struct, sys, numpy as np
from loadpth import load
a = load(sys.argv[1]); a = a.get('params', a)
b = load(sys.argv[2]); b = b.get('params', b)
dn = 0.5
W = {k: ((1 - dn) * a[k].astype(np.float32) + dn * b[k].astype(np.float32)) for k in a}
layers = [(0, 1)] + [(i, i + 1) for i in range(2, 65, 2)] + [(66, None)]
hdr = struct.pack('<4sII', b'LBSR', 1, len(layers)); data = []
for ci, pi in layers:
    w = W['body.%d.weight' % ci]; cout, cin = w.shape[:2]
    hdr += struct.pack('<III', cin, cout, 1 if pi else 0)
    data += [w.ravel(), W['body.%d.bias' % ci].ravel()] + ([W['body.%d.weight' % pi].ravel()] if pi else [])
blob = np.concatenate(data).astype(np.float16).tobytes()
open(sys.argv[3], 'wb').write(hdr + blob)
print(len(layers), 'layers', len(hdr) + len(blob), 'bytes')
