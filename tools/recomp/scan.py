import sys, time
from analyze import Image, discover
data = open(sys.argv[1], "rb").read()
img = Image(data, 0x8C010000)
t=time.time()
funcs = discover(img, [0x8C010000])
good = [f for f in funcs.values() if not f.bad]
bad = [f for f in funcs.values() if f.bad]
code = sum(len(f.insns) for f in good)*2
print(f"{len(good)} functions ok, {len(bad)} bad, code bytes {code} ({code/len(data)*100:.1f}%), {time.time()-t:.1f}s")
ind = sum(len(f.indirect) for f in good)
print("unresolved indirect jumps/calls:", ind)
for f in bad[:15]: print(hex(f.entry), f.bad)
