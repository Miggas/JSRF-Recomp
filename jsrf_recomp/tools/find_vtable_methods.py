import json,struct,re,capstone
exec(open('xdis.py').read().split('md=')[0].replace('import sys,struct,capstone','import struct'))
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
disp=set(int(m,16) for m in re.findall(r'\{ 0x([0-9A-F]{8})u', open('jsrf_recomp/src/recomp/gen/recomp_dispatch.c').read()))
seeds={int(x['start'],16) for x in json.load(open('jsrf_recomp/analysis/seeds.json'))}
tlo,thi=0x11000,0x11000+0x17bb30
cands={}
for name,(va,vs,ra,rs) in zip(['.text','D3D','DSOUND','MMATRIX','XGRPH','XPP','.rdata','.data'],secs[:8]):
    if name not in ('.rdata','.data'): continue
    b=d[ra:ra+rs]
    for i in range(0,len(b)-3,4):
        p=struct.unpack_from('<I',b,i)[0]
        if tlo<=p<thi and p not in disp and p not in seeds:
            cands.setdefault(p,[]).append(va+i)
# keep those that look like vtable members: neighbours (+-4) are also code pointers in dispatch
good=[]
for p,refs in cands.items():
    ok=False
    for r in refs:
        for dlt in (-4,4):
            q=struct.unpack_from('<I',rd(r+dlt,4))[0] if rd(r+dlt,4) else 0
            if q in disp: ok=True
    if not ok: continue
    ins=list(md.disasm(rd(p,64),p))
    if len(ins)<3: continue
    m0=ins[0].mnemonic+' '+ins[0].op_str
    bad=any(x.mnemonic in ('in','out','arpl','bound','les','lds','hlt','into','daa','das','aaa','aas','salc') or 'byte ptr [eax], al' in x.op_str for x in ins[:6])
    if bad: continue
    good.append(p)
good.sort()
print(len(cands),len(good)); print(' '.join(hex(x) for x in good[:80]))
json.dump(good,open('vptrs.json','w'))
