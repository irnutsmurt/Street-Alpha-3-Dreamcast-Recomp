"""Dreamcast counterparts of PSP addresses that characters' data points at:
find old characters whose PSP data holds a pointer to the address, and read the
word at the same place in their Dreamcast character file (the tables have the
same shape on both machines).

    python3 tools/psp/resolve_data.py <Dreamcast files dir> <PSP BOOT.BIN> <hex address>...
"""
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "recomp"))
from mipsgen import Boot  # noqa: E402
from build_char import descriptors, DESC_WORDS  # noqa: E402
from align import Dc  # noqa: E402

B = 0x8C800000


def resolve(dc_dir, boot_path, targets):
    b = Boot(boot_path)
    ds = descriptors(b)
    refs = collections.defaultdict(list)  # target -> PSP data addresses holding it
    tset = set(targets)
    for a in b.data_ptrs:
        v = b.u32(a)
        if v in tset:
            refs[v].append(a)
    # each old character's descriptor tables, walked side by side with the Dreamcast's
    out = {t: collections.Counter() for t in targets}
    for cid in [i for i in range(0x26) if i != 0x13]:
        path = os.path.join(dc_dir, "PL%02XA.BIN" % cid)
        dc = Dc(path)
        dw = dc.descriptor()
        pa, pw = ds[cid]
        pairs, seen = [], set()
        todo = [(dw[k], pw[k]) for k in range(DESC_WORDS)]
        while todo:
            x, y = todo.pop()
            if (x, y) in seen or not dc.img.contains(x) or not 0x1FEDE0 <= y < 0x2D4F30:
                continue
            seen.add((x, y))
            for k in range(256):
                if not dc.img.contains(x + 4 * k + 3):
                    break
                dv, pv = dc.img.u32(x + 4 * k), b.u32(y + 4 * k)
                if pv in tset and (y + 4 * k) in b.data_ptrs:
                    out[pv][dv] += 1
                if dc.img.contains(dv) and (y + 4 * k) in b.data_ptrs and 0x1FEDE0 <= pv < 0x2D4F30:
                    todo.append((dv, pv))
                elif (dv == 0) != (pv == 0):
                    break
    return refs, out


if __name__ == "__main__":
    dc_dir, boot = sys.argv[1:3]
    targets = [int(x, 16) for x in sys.argv[3:]]
    refs, out = resolve(dc_dir, boot, targets)
    for t in targets:
        print(f"{t:06X}: referenced {len(refs[t])}x;", ", ".join(f"{d:08X} ({n})" for d, n in out[t].most_common(3)) or "no Dreamcast match")
