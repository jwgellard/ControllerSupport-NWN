import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for a in sys.argv[2:]:
    va = int(a, 16); n = 80
    if ':' in a: pass
    code = pe.get_data(va - base, 120)
    print('==', a)
    for i, ins in enumerate(md.disasm(code, va)):
        print('   ', hex(ins.address), ins.mnemonic, ins.op_str)
        if ins.mnemonic in ('ret','int3') or i > 30: break
