# dis.py <exe> <start: hex VA or export name> <count> [ -sym ]  -- disassemble with SDL/export names annotated
import pefile, struct, sys, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[0]
f = struct.unpack('<IIHHIIIIIII', pe.get_data(d.VirtualAddress, 40))
nn = f[7]
names = struct.unpack('<%dI' % nn, pe.get_data(f[9], 4 * nn))
ords = struct.unpack('<%dH' % nn, pe.get_data(f[10], 2 * nn))
funcs = struct.unpack('<%dI' % f[6], pe.get_data(f[8], 4 * f[6]))
ex = {pe.get_string_at_rva(n).decode(): funcs[o] for n, o in zip(names, ords)}
byaddr = {base + r: n for n, r in ex.items()}
s = sys.argv[2]
va = byaddr and (base + ex[s] if s in ex else int(s, 16))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = False
n = int(sys.argv[3])
for k, ins in enumerate(md.disasm(pe.get_data(va - base, n * 15), va)):
    if k >= n:
        break
    note = ''
    try:
        t = int(ins.op_str, 16)
        if t in byaddr:
            note = '   ; ' + byaddr[t][:90]
    except ValueError:
        pass
    print('%x  %-6s %s%s' % (ins.address, ins.mnemonic, ins.op_str, note))
