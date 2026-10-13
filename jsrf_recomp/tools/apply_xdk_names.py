import re,sys,json,collections
names={}
for l in open(sys.argv[1]):
    m=re.match(r"(\w+?)__FUN__(?:\w+?__)?(\w+)\(.*\) = 0x([0-9a-f]+)",l.strip())
    if m: names[int(m.group(3),16)]=f"{m.group(1)}_{m.group(2)}"
fs=json.load(open(sys.argv[2])); st={int(f["start"],16):f for f in fs}; used=collections.Counter()
for a,n in names.items():
    if a in st: used[n]+=1; st[a]["name"]=n if used[n]==1 else f"{n}_{a:08X}"
json.dump(fs,open(sys.argv[2],"w"),indent=1)
