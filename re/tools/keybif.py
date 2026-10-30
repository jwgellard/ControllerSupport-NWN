# Minimal NWN KEY/BIF V1 extractor: keybif.py <data_dir> <keyfile> <resref> <restype> <out>
import struct, sys, os

data_dir, keyname, want, wtype, out = sys.argv[1], sys.argv[2], sys.argv[3].lower(), int(sys.argv[4]), sys.argv[5]
k = open(os.path.join(data_dir, keyname), 'rb').read()
sig, ver, nbif, nkey, offbif, offkey = struct.unpack_from('<4s4sIIII', k, 0)
bifs = []
for i in range(nbif):
    size, noff, nlen = struct.unpack_from('<IIH', k, offbif + i * 12)
    bifs.append(k[noff:noff + nlen].rstrip(b'\0').decode().replace('\\', '/'))
for i in range(nkey):
    ref, rtype, rid = struct.unpack_from('<16sHI', k, offkey + i * 22)
    ref = ref.rstrip(b'\0').decode().lower()
    if ref == want and rtype == wtype:
        bif = bifs[rid >> 20]
        idx = rid & 0xFFFFF
        b = open(os.path.join(os.path.dirname(data_dir), bif), 'rb').read()
        _, _, nvar, _, offvar = struct.unpack_from('<4s4sIII', b, 0)
        for j in range(nvar):
            vid, off, sz, t = struct.unpack_from('<IIII', b, offvar + j * 16)
            if (vid & 0xFFFFF) == idx:
                open(out, 'wb').write(b[off:off + sz])
                print('extracted', want, 'from', bif, sz, 'bytes')
                sys.exit(0)
print('not found'); sys.exit(1)
