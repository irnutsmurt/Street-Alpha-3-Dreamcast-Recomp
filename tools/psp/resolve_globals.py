"""Find the Dreamcast counterparts of particular PSP globals: through the
engine functions (already paired) that use them, lined up event by event.

    python3 tools/psp/resolve_globals.py <Dreamcast files dir> <PSP BOOT.BIN> <maps.json> <hex address>...
"""
import collections
import difflib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from align import DcMain, Psp, kinds  # noqa: E402


def resolve(dc_dir, boot, maps, targets):
    psp = Psp(boot)
    main = DcMain(os.path.join(dc_dir, "1ST_READ.BIN"))
    fmap = {int(k, 16): int(v, 16) for k, v in json.load(open(maps))["functions"].items()}
    votes = {t: collections.Counter() for t in targets}
    for p, d in fmap.items():
        pe = psp.events(p)
        hits = [k for k, e in enumerate(pe) if e[0] == "G" and any(abs(e[1] - t) < 0x100 for t in targets)]
        if not hits:
            continue
        de = main.events(d)
        sm = difflib.SequenceMatcher(None, kinds(de), kinds(pe), autojunk=False)
        for blk in sm.get_matching_blocks():
            for k in range(blk.size):
                x, y = de[blk.a + k], pe[blk.b + k]
                if y[0] == "G" and x[0] == "G":
                    for t in targets:
                        if 0 <= t - y[1] < 0x100 or 0 <= y[1] - t < 0x100:
                            votes[t][x[1] + (t - y[1])] += 1  # same offset from the accessed address
    return votes


if __name__ == "__main__":
    dc_dir, boot, maps = sys.argv[1:4]
    targets = [int(x, 16) for x in sys.argv[4:]]
    for t, c in resolve(dc_dir, boot, maps, targets).items():
        print(f"{t:06X}:", ", ".join(f"{d:08X} ({n})" for d, n in c.most_common(4)) or "nothing")
