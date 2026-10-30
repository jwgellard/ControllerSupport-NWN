import sys, struct, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True); b = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
data = text.get_data(); tva = b + text.VirtualAddress
for tgt in [int(x, 16) for x in sys.argv[2:]]:
    print('== callers of', hex(tgt))
    for i in range(len(data) - 5):
        if data[i] in (0xE8, 0xE9) and tva + i + 5 + struct.unpack_from('<i', data, i + 1)[0] == tgt:
            at = tva + i
            ctx = list(md.disasm(data[i - 40:i + 5], at - 40))
            print('  %x' % at, ' | '.join('%s %s' % (x.mnemonic, x.op_str) for x in ctx[-6:]))
