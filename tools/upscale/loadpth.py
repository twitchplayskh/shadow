import zipfile, pickle, numpy as np, collections
def load(path):
    z = zipfile.ZipFile(path); names = z.namelist(); root = names[0].split('/')[0]
    class Storage:
        def __init__(s, key, dtype): s.key, s.dtype = key, dtype
    def rebuild(storage, offset, size, stride, *a):
        raw = np.frombuffer(z.read('%s/data/%s' % (root, storage.key)), dtype=storage.dtype)
        n = int(np.prod(size)) if size else 1
        arr = raw[offset:offset + n] if n else raw[:0]
        return arr.reshape(size).copy() if size else arr.copy()
    class U(pickle.Unpickler):
        def find_class(s, mod, name):
            if name == '_rebuild_tensor_v2': return rebuild
            if name == 'OrderedDict': return collections.OrderedDict
            if name.endswith('Storage'): return {'FloatStorage': np.float32, 'HalfStorage': np.float16}[name]
            return super().find_class(mod, name)
        def persistent_load(s, pid):
            _, dt, key, loc, n = pid
            return Storage(key, dt)
    d = U(z.open('%s/data.pkl' % root)).load()
    return d
if __name__ == '__main__':
    import sys
    d = load(sys.argv[1]); d = d.get('params', d)
    tot = 0
    for k, v in d.items(): tot += v.size; 
    ks = list(d.keys()); print(len(ks), tot); print(ks[:6], ks[-4:]); print([d[k].shape for k in ks[:4]], [d[k].shape for k in ks[-3:]])
