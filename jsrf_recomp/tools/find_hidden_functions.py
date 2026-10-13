import json,struct,capstone,re
exec(open('xdis.py').read().split('md=')[0].replace('import sys,struct,capstone','import struct'))
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
f=json.load(open('jsrf_recomp/analysis/functions_named.json'))
starts={int(x['start'],16) for x in f}
disp=set(int(m,16) for m in re.findall(r'\{ 0x([0-9A-F]{8})u', open('jsrf_recomp/src/recomp/gen/recomp_dispatch.c').read()))
seeds={int(x['start'],16) for x in json.load(open('jsrf_recomp/analysis/seeds.json'))}
PRO=('push ebp','sub esp','push ebx','push esi','push edi','push ecx','mov eax, dword ptr fs:','push -1','mov ecx, dword ptr [esp','mov eax, dword ptr [esp','mov edx, dword ptr [esp','mov eax, dword ptr [ecx','mov eax, dword ptr [0x','push 0x')
out=[]
BAD=('adc','sbb','in','out','arpl','bound','les','lds','hlt','into','aaa','aas','daa','das','salc','sahf','lahf','cli','sti','iretd','retf','insb','insd','outsb','outsd','enter','loop','loopne','loope','jecxz','xlatb','int','fwait')
def plausible(a):
    n=0; pushes=[]
    for ins in md.disasm(rd(a,512),a):
        m=ins.mnemonic; n+=1
        if m in BAD or 'byte ptr [eax], al' in ins.op_str: return False
        if m=='push' and ins.op_str in ('eax','ebx','ecx','edx','esi','edi','ebp'): pushes.append(ins.op_str)
        if m=='pop' and n<=3 and pushes and pushes[-1]==ins.op_str: return False
        if m in ('ret','jmp'): return n>=3
        if n>=200: return True
    return False
prev_mem=False
for x in f:
    s=int(x['start'],16); e=int(x['end'],16)
    if e-s<16 or e-s>200000: continue
    if not (0x11000<=s<0x1BC7C0+0x7798): continue
    code=rd(s,e-s)
    prev=None; pad=False
    for ins in md.disasm(code,s):
        m=ins.mnemonic
        if (prev in ('ret',) or (prev=='jmp' and not prev_mem)) and m in ('nop','int3'):
            pad=True
        elif pad and ins.address%16==0 and ins.address not in disp and ins.address not in seeds:
            t=(m+' '+ins.op_str)
            if any(t.startswith(p) for p in PRO) and plausible(ins.address): out.append(ins.address)
            pad=False
        elif m not in ('nop','int3'):
            pad=False
        if m not in ('nop','int3'): prev=m; prev_mem=('ptr' in ins.op_str)
print(len(out)); print(' '.join(hex(a) for a in out[:60]))
json.dump(out,open('hidden.json','w'))
