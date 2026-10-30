import sys, pefile, struct
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
def q(va): return struct.unpack_from("<Q", img, va-base)[0]
names = {}
for line in open(sys.argv[2], encoding="utf-8", errors="replace"):
    p = line.rstrip("\n").split("\t")
    if len(p) >= 2:
        try: names[int(p[0],16)] = p[1]
        except: pass
for spec in sys.argv[3:]:
    name, va, n = spec.split(":")
    va = int(va,16)
    print("==", name, hex(va))
    for i in range(int(n)):
        f = q(va+8*i)
        print("  %3d +0x%03x %x %s" % (i, 8*i, f, names.get(f,"")))
