import sys
from analyze import Image, discover
data = open(sys.argv[1], "rb").read()
img = Image(data, 0x8C010000)
funcs = discover(img, [0x8C010000])
addrs = sorted(a for f in funcs.values() for a in f.insns)
# coalesce into regions with gap<4KB
regs=[]; s=p=addrs[0]
for a in addrs[1:]:
    if a-p>4096: regs.append((s,p)); s=a
    p=a
regs.append((s,p))
for s,e in regs: print(f"{s:08X}-{e:08X} {(e-s)//1024}KB")
