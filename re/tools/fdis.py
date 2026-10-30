import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True); b = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
va = int(sys.argv[2], 16); n = int(sys.argv[3]); flt = sys.argv[4] if len(sys.argv) > 4 else None
code = pe.get_data(va - b, n)
for ins in md.disasm(code, va):
    s = '%x %s %s' % (ins.address, ins.mnemonic, ins.op_str)
    if not flt or flt in s: print(s)
