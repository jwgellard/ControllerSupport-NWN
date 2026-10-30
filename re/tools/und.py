import ctypes
db=ctypes.WinDLL('dbghelp')
buf=ctypes.create_string_buffer(2048)
out=open('exports.txt','w')
for l in open('exports_raw.txt'):
    a,n=l.rstrip('\n').split('\t')
    u=n
    if n.startswith('?'):
        if db.UnDecorateSymbolName(n.encode(),buf,2048,0x1000|0)  : u=buf.value.decode(errors='replace')
    out.write(f"{a}\t{u}\t{n}\n")
