import sys, pefile, capstone
pefile.MAX_SYMBOL_EXPORT_COUNT = 200000
pe = pefile.PE(sys.argv[1])
print('ImageBase %x' % pe.OPTIONAL_HEADER.ImageBase)
print('== imports ==')
for e in pe.DIRECTORY_ENTRY_IMPORT:
    names = [i.name.decode() for i in e.imports if i.name]
    sdl = [n for n in names if 'SDL' in n]
    print(e.dll.decode(), len(names), sdl[:5])
exps = {s.name.decode(): s.address for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if s.name}
print('== %d exports, %d SDL_ ==' % (len(exps), sum(1 for n in exps if n.startswith('SDL_'))))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
base = pe.OPTIONAL_HEADER.ImageBase
for name in sys.argv[2:]:
    rva = exps.get(name)
    if rva is None:
        print(name, 'NOT EXPORTED'); continue
    code = pe.get_data(rva, 48)
    print('--', name, hex(base + rva))
    for i, ins in enumerate(md.disasm(code, base + rva)):
        print('   ', hex(ins.address), ins.mnemonic, ins.op_str)
        if i > 6 or ins.mnemonic in ('jmp', 'ret'):
            break
