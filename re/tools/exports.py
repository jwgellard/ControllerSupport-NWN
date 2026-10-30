import sys, struct, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
b = pe.OPTIONAL_HEADER.ImageBase
d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[0]
ed = pe.get_data(d.VirtualAddress, 40)
nfun, nnames, afun, anames, aords = struct.unpack_from('<IIIII', ed, 20)
names = pe.get_data(anames, nnames*4); ords = pe.get_data(aords, nnames*2); funs = pe.get_data(afun, nfun*4)
pats = [p.encode() for p in sys.argv[2:]]
for i in range(nnames):
    nr = struct.unpack_from('<I', names, i*4)[0]
    nm = pe.get_string_at_rva(nr, 512)
    if any(p in nm for p in pats):
        o = struct.unpack_from('<H', ords, i*2)[0]
        print(nm.decode(), hex(b + struct.unpack_from('<I', funs, o*4)[0]))
