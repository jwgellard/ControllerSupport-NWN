import sys, pefile, capstone
pe = pefile.PE(sys.argv[1])
base = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if e.name in (b"SDL_PollEvent", b"SDL_GetTicks", b"SDL_DYNAPI_entry"):
        data = pe.get_memory_mapped_image()[e.address:e.address+48]
        print(e.name, hex(base+e.address))
        for i in list(md.disasm(data, base+e.address))[:8]:
            print("   ", hex(i.address), i.bytes.hex(), i.mnemonic, i.op_str)
