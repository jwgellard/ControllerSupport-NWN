# Find every direct call/jmp into an exported SDL_* stub and attribute it to the
# nearest preceding exported symbol (NWN exports its own C++ functions too).
import pefile, struct, bisect, sys, re, collections
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[0]
f = struct.unpack('<IIHHIIIIIII', pe.get_data(d.VirtualAddress, 40))
nn = f[7]
names = struct.unpack('<%dI' % nn, pe.get_data(f[9], 4 * nn))
ords = struct.unpack('<%dH' % nn, pe.get_data(f[10], 2 * nn))
funcs = struct.unpack('<%dI' % f[6], pe.get_data(f[8], 4 * f[6]))
ex = {pe.get_string_at_rva(n).decode(): funcs[o] for n, o in zip(names, ords)}
sdl = {rva: n for n, rva in ex.items() if n.startswith('SDL_')}
syms = sorted((rva, n) for n, rva in ex.items() if not n.startswith('SDL_'))
srvas = [s[0] for s in syms]
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
data = text.get_data()
tva = text.VirtualAddress
filt = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
res = collections.defaultdict(set)
for m in re.finditer(rb'[\xe8\xe9]', data):
    i = m.start()
    if i + 5 > len(data):
        break
    tgt = (tva + i + 5 + struct.unpack_from('<i', data, i + 1)[0]) & 0xffffffff
    if tgt in sdl:
        fn = sdl[tgt]
        if filt and not filt.search(fn):
            continue
        k = bisect.bisect_right(srvas, tva + i) - 1
        owner = syms[k][1] if k >= 0 else '?'
        res[fn].add('%s (+0x%x)' % (owner, tva + i - syms[k][0]))
for fn in sorted(res):
    print(fn)
    for o in sorted(res[fn])[:12]:
        print('    ', o)
