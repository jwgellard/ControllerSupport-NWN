import re,bisect,pefile,capstone
pe=pefile.PE('../../nwmain.exe',fast_load=True); base=0x140000000
starts=[]
sizes={}
for l in open('fa2.txt'):
    m=re.match(r'[0-9a-f]+ -> ([0-9a-f]+) .* size=([0-9]+)',l)
    if m: sizes[int(m.group(1),16)]=int(m.group(2))
for l in open('syms.txt',encoding='utf8'):
    p=l.rstrip('\n').split('\t')
    if len(p)==3 and p[1]=='Function' and ':' not in p[0] and not p[2].startswith(('Unwind','Catch','Try','Handler')): starts.append(int(p[0],16))
starts=sorted(set(starts))
T=0x1407c25c0
callers=sorted({int(m.group(1),16) for l in open('c1.txt') if 'UNCONDITIONAL_CALL' in l and (m:=re.search(r'in ([0-9a-f]+) FUN',l))})
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); md.detail=False
out=[]
for f in callers:
    end=f+sizes[f]
    code=pe.get_data(f-base,end-f)
    ins=list(md.disasm(code,f))
    pairs=[]
    for k,x in enumerate(ins):
        if x.mnemonic=='call' and x.op_str in ('0x1407c25c0',hex(T)):
            maj=mn=None
            for y in reversed(ins[max(0,k-14):k]):
                m=re.match(r'(edx|dl), (0x[0-9a-f]+|\d+)$',y.op_str)
                if m and y.mnemonic=='mov' and maj is None: maj=int(m.group(2),0)
                m=re.match(r'(r8d|r8b), (0x[0-9a-f]+|\d+)$',y.op_str)
                if m and y.mnemonic=='mov' and mn is None: mn=int(m.group(2),0)
            pairs.append((maj,mn))
    out.append((f,end-f,pairs))
for f,sz,p in out: print(hex(f),sz,p)
