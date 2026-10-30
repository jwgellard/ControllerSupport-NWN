import sys, struct, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True); b = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
data = text.get_data(); tva = b + text.VirtualAddress
for tgt in [int(x, 16) for x in sys.argv[2:]]:
    print('== refs to', hex(tgt))
    for i in range(len(data) - 7):
        # rip-relative lea/mov: disp32 at i+3 for 7-byte insn
        if data[i] in (0x48, 0x4c) and data[i+1] in (0x8d, 0x8b) and (data[i+2] & 0xc7) == 0x05:
            if tva + i + 7 + struct.unpack_from('<i', data, i + 3)[0] == tgt:
                print('  %x' % (tva + i))
