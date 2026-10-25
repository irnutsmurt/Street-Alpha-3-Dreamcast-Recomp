"""Find the Dreamcast engine's tables indexed by a character number.

A character number comes from a fighter's +0x132 (or a function that returns
one). It's followed through register moves, scaling (shifts, multiplies) and
calls (a function given one as an argument is analysed with that argument
marked; one that returns it marks its callers' r0), to the table reads it
indexes: @(R0,Rm) with the other register a table address from a literal, or
add-then-read. Prints each table with its entry scales and the code using it.

    python3 tools/psp/dc_idtables.py <Dreamcast files dir> <generated C dir> [out.json]
"""
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "recomp"))
import sh4  # noqa: E402
from analyze import Image, explore  # noqa: E402

CHAR_ID = 0x132
DATA_LO, DATA_HI = 0x8C090000, 0x8D000000  # where tables live (data and work RAM)
CMPS = {"cmp/eq", "cmp/hs", "cmp/ge", "cmp/hi", "cmp/gt", "tst", "cmp/eq #imm", "cmp/pz", "cmp/pl"}
NOWRITE = CMPS | {"bt", "bf", "bt/s", "bf/s", "bra", "bsr", "jsr", "jmp", "rts", "nop", "mov.b st", "mov.w st",
                  "mov.l st", "mov.b r0st", "mov.w r0st", "mov.l r0st", "mov.l stdisp", "mov.b r0stdisp",
                  "mov.w r0stdisp", "mov.b stdec", "mov.w stdec", "mov.l stdec"}
SHIFT = {"shll": 2, "shll2": 4, "shll8": 256, "shll16": 65536, "add": None}


def functions(gen_dir):
    names = set()
    for f in os.listdir(gen_dir):
        if f.startswith("code_") and f.endswith(".c"):
            for m in re.finditer(r"^void f_(8C[0-9A-F]{6})\(void\) \{", open(os.path.join(gen_dir, f)).read(), re.M):
                names.add(int(m.group(1), 16))
    return sorted(names)


class Analysis:
    def __init__(self, img):
        self.img = img
        self.cache = {}
        self.tables = collections.defaultdict(list)  # table -> [(scale, site)]
        self.returns_id = set()
        self.arg_taint = collections.defaultdict(set)  # function -> {arg regs}
        self.calls_with = []  # (callee, regs) found this pass

    def fn(self, e):
        if e not in self.cache:
            try:
                f = explore(self.img, e, set())
                self.cache[e] = None if f.bad else f
            except Exception:
                self.cache[e] = None
        return self.cache[e]

    def lit(self, a, size):
        if size == 4:
            return self.img.u32(a)
        v = self.img.u16(a)
        return (v - 0x10000 if v & 0x8000 else v) & 0xFFFFFFFF

    def scan(self, e, tainted_args=()):
        f = self.fn(e)
        if not f:
            return False
        img = self.img
        known = {}
        idx = {r: 1 for r in tainted_args}
        ret_tainted = False
        for a in sorted(f.insns):
            if a in f.labels and a != e:
                known = {}
                idx = {}
            i = f.insns[a]
            n = i.name
            if n == "mov.l@pc":
                known[i.n] = self.lit(sh4.pcrel_addr(i, a), 4); idx.pop(i.n, None); continue
            if n == "mov.w@pc":
                known[i.n] = self.lit(sh4.pcrel_addr(i, a), 2) & 0xFFFF; idx.pop(i.n, None); continue
            if n == "mov #imm":
                known[i.n] = (i.i - 0x100 if i.i & 0x80 else i.i) & 0xFFFFFFFF; idx.pop(i.n, None); continue
            if n in ("mov.b r0ld", "mov.w r0ld", "mov.l r0ld"):
                r0, rm = known.get(0), i.m
                if r0 == CHAR_ID and n == "mov.b r0ld":
                    idx[i.n] = 1; known.pop(i.n, None); continue
                if 0 in idx and DATA_LO <= known.get(rm, 0) < DATA_HI:
                    self.tables[known[rm]].append((idx[0], a))
                elif rm in idx and DATA_LO <= known.get(0, 0) < DATA_HI:
                    self.tables[known[0]].append((idx[rm], a))
                known.pop(i.n, None); idx.pop(i.n, None)
                continue
            if n in ("mov.b ld", "mov.w ld", "mov.l ld"):  # @Rm: an earlier add made Rm table + index
                known.pop(i.n, None); idx.pop(i.n, None); continue
            if n in ("extu.b", "exts.b", "extu.w", "exts.w", "mov"):
                if i.m in idx:
                    idx[i.n] = idx[i.m]
                else:
                    idx.pop(i.n, None)
                if n == "mov" and i.m in known:
                    known[i.n] = known[i.m]
                else:
                    known.pop(i.n, None)
                continue
            if n in SHIFT:
                if n == "add":
                    if i.m in idx and DATA_LO <= known.get(i.n, 0) < DATA_HI:
                        self.tables[known[i.n]].append((idx[i.m], a)); idx.pop(i.n, None); known.pop(i.n, None)
                    elif i.n in idx and DATA_LO <= known.get(i.m, 0) < DATA_HI:
                        self.tables[known[i.m]].append((idx[i.n], a)); idx.pop(i.n, None); known.pop(i.n, None)
                    elif i.m in idx and i.n in idx:
                        idx[i.n] = idx[i.n] + idx[i.m]
                    elif i.n in idx and i.m in known and known[i.m] < 0x10000:
                        pass  # id + a small constant: still an index
                    else:
                        idx.pop(i.n, None); known.pop(i.n, None)
                    continue
                if i.n in idx:
                    idx[i.n] *= SHIFT[n]
                known.pop(i.n, None)
                continue
            if n == "mulu.w" and (i.n in idx or i.m in idx):
                k = known.get(i.m if i.n in idx else i.n)
                macl = (idx.get(i.n) or idx.get(i.m)) * (k if k is not None and k < 0x10000 else 0)
                idx["macl"] = macl or 1
                continue
            if n == "sts macl":
                if "macl" in idx:
                    idx[i.n] = idx["macl"]
                else:
                    idx.pop(i.n, None)
                known.pop(i.n, None)
                continue
            if n in ("jsr", "bsr"):
                target = known.get(i.n) if n == "jsr" else sh4.branch_target(i, a)
                regs = tuple(r for r in (4, 5, 6, 7) if r in idx and idx[r] == 1)
                if target is not None and regs:
                    self.calls_with.append((target, regs))
                had_ret = target in self.returns_id
                for r in range(8):
                    idx.pop(r, None); known.pop(r, None)
                if had_ret:
                    idx[0] = 1
                continue
            if n == "rts":
                if 0 in idx and idx[0] == 1:
                    ret_tainted = True
                continue
            if n not in NOWRITE:
                idx.pop(i.n, None)
                known.pop(i.n, None)
            if n in ("mov.b r0lddisp", "mov.w r0lddisp"):
                known.pop(0, None); idx.pop(0, None)
        return ret_tainted

    def run(self, entries):
        for _ in range(4):  # a few rounds: arguments and returns spread along calls
            before = (len(self.returns_id), sum(len(v) for v in self.arg_taint.values()))
            self.tables.clear()
            self.calls_with = []
            for e in entries:
                if self.scan(e):
                    self.returns_id.add(e)
                for regs in [()] + [tuple(sorted(self.arg_taint[e]))] if self.arg_taint.get(e) else [()]:
                    if regs and self.scan(e, regs):
                        self.returns_id.add(e)
            for t, regs in self.calls_with:
                self.arg_taint[t] |= set(regs)
            for e in list(self.arg_taint):
                if self.scan(e, tuple(sorted(self.arg_taint[e]))):
                    self.returns_id.add(e)
            after = (len(self.returns_id), sum(len(v) for v in self.arg_taint.values()))
            if after == before:
                break


def main():
    dc_dir, gen_dir = sys.argv[1:3]
    img = Image(open(os.path.join(dc_dir, "1ST_READ.BIN"), "rb").read(), 0x8C010000)
    an = Analysis(img)
    an.run(functions(gen_dir))
    out = {}
    for t in sorted(an.tables):
        scales = collections.Counter(s for s, a in an.tables[t])
        out[f"{t:08X}"] = {"scales": dict(scales), "sites": [f"{a:08X}" for s, a in an.tables[t][:8]]}
        print(f"{t:08X} scale {dict(scales)} sites {' '.join(f'{a:08X}' for s, a in an.tables[t][:4])}")
    print(len(an.tables), "tables;", len(an.returns_id), "functions return a character number;",
          len(an.arg_taint), "take one")
    if len(sys.argv) > 3:
        json.dump(out, open(sys.argv[3], "w"), indent=1)


if __name__ == "__main__":
    main()
