import struct,sys,subprocess
d=open(sys.argv[1],'rb').read()
e=struct.unpack_from('<I',d,0x3c)[0]
nsec=struct.unpack_from('<H',d,e+6)[0]; osz=struct.unpack_from('<H',d,e+20)[0]
secs=[struct.unpack_from('<4xIII',d,e+24+osz+i*40) for i in range(nsec)]
def r2o(rva):
    for vs,va,rs in [(s[0],s[1],s[2]) for s in secs]:
        pass
    for i in range(nsec):
        o=e+24+osz+i*40
        vs,va,rs,rp=struct.unpack_from('<IIII',d,o+8)
        if va<=rva<va+max(vs,rs): return rva-va+rp
dd=struct.unpack_from('<II',d,e+24+112)
ed=r2o(dd[0])
nf,nn=struct.unpack_from('<II',d,ed+20); af,an,ao=struct.unpack_from('<III',d,ed+28)
base=struct.unpack_from('<I',d,ed+16)[0]
out=open(sys.argv[2],'w')
for i in range(nn):
    no=struct.unpack_from('<I',d,r2o(an)+4*i)[0]
    o=r2o(no); n=d[o:d.index(b'\0',o)].decode()
    ordn=struct.unpack_from('<H',d,r2o(ao)+2*i)[0]
    f=struct.unpack_from('<I',d,r2o(af)+4*ordn)[0]
    out.write(f"{0x140000000+f:x}\t{n}\n")
