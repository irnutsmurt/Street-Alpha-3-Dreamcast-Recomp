"""Line up the PSP's character code (MIPS) with the Dreamcast's (SH-4).

Both were compiled from the same source, so a character's functions do the
same things in much the same order on both machines. Each function is boiled
down to a sequence of events: memory accesses through a pointer (load/store,
size, offset), accesses to fixed addresses (globals) and calls. Matching
functions are paired (from the character descriptors, then through the calls
they make), their event sequences aligned, and every aligned pair votes:
  struct offsets  Dreamcast offset -> PSP offset (the PSP inserted fields)
  globals         Dreamcast address -> PSP address
  functions       Dreamcast function -> PSP function (the engine's API)

    python3 tools/psp/align.py <dreamcast files dir> <PSP BOOT.BIN> [out.json]
"""
import collections
import difflib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "recomp"))
sys.path.insert(0, HERE)

import mips  # noqa: E402
import sh4  # noqa: E402
from analyze import Image, discover, explore  # noqa: E402

OVERLAY_BASE = 0x8C800000
PSP_OFF = 0x74          # BOOT.BIN: file offset = address + 0x74
DESC_WORDS = 39
CODE_WORDS = range(16, 28)


# ---------------- PSP side ----------------

class Psp:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        self.text_end = 0x1F8354

    def u32(self, a):
        return struct.unpack_from("<I", self.data, a + PSP_OFF)[0]

    def insn(self, a):
        return mips.decode(self.u32(a), a)

    def function(self, entry):
        """Reachable instructions from entry (address order), and its local branch targets."""
        seen, labels, todo = set(), set(), [entry]
        while todo:
            a = todo.pop()
            while a not in seen and a < self.text_end:
                seen.add(a)
                i = self.insn(a)
                if i.flow == mips.FLOW_NONE or i.flow in (mips.FLOW_CALL, mips.FLOW_CALL_REG):
                    a += 4
                    continue
                seen.add(a + 4)  # delay slot
                if i.flow in (mips.FLOW_BRANCH, mips.FLOW_BRANCH_LIKELY):
                    labels.add(i.target)
                    todo.append(i.target)
                    a += 8
                    continue
                if i.flow == mips.FLOW_JUMP and i.name == "b":
                    labels.add(i.target)
                    a = i.target
                    continue
                break  # jr / j (tail call): end of this path
        return sorted(seen), labels

    def events(self, entry):
        addrs, labels = self.function(entry)
        known, out, tables, entries, delta = {0: 0}, [], {}, {}, {}
        for a in addrs:
            if a in labels:
                known, tables, entries, delta = {0: 0}, {}, {}, {}
            i = self.insn(a)
            n = i.name
            if n == "lui":
                known[i.rt] = (i.imm << 16) & 0xFFFFFFFF
                continue
            if n in ("addiu", "addi"):
                if i.rs in known:
                    known[i.rt] = (known[i.rs] + i.imm) & 0xFFFFFFFF
                else:
                    known.pop(i.rt, None)
                    if i.rs != 29:  # pointer + constant
                        delta[i.rt] = delta.get(i.rs, 0) + i.imm
                        tables.pop(i.rt, None)
                        entries.pop(i.rt, None)
                        continue
                delta.pop(i.rt, None)
                continue
            if n == "ori":
                if i.rs in known:
                    known[i.rt] = known[i.rs] | i.imm
                else:
                    known.pop(i.rt, None)
                continue
            if n == "or" and i.rt == 0 and i.rs in known:
                known[i.rd] = known[i.rs]
                continue
            if n == "or" and i.rt == 0 and i.rd:  # move: a pointer + constant stays one
                known.pop(i.rd, None)
                if i.rs in delta:
                    delta[i.rd] = delta[i.rs]
                else:
                    delta.pop(i.rd, None)
                continue
            if n == "addu" and i.rd:
                x, y = known.get(i.rs), known.get(i.rt)
                delta.pop(i.rd, None)
                if isinstance(x, int) and x > 0x1000 and y is None:
                    tables[i.rd] = x  # table base + an index
                elif isinstance(y, int) and y > 0x1000 and x is None:
                    tables[i.rd] = y
                else:
                    tables.pop(i.rd, None)
                    if isinstance(y, int) and x is None and y < 0x1000:
                        delta[i.rd] = delta.get(i.rs, 0) + y  # pointer + a constant offset
                    elif isinstance(x, int) and y is None and x < 0x1000:
                        delta[i.rd] = delta.get(i.rt, 0) + x
                known.pop(i.rd, None)
                continue
            if n in mips.MEM.values():
                size = {"b": 1, "h": 2, "w": 4}.get(n[1], 4) if n[0] in "ls" and n not in ("lwc1", "swc1") else 4
                kind = "S" if n[0] == "s" else "L"
                if i.rs in known:
                    out.append(("G", (known[i.rs] + i.imm) & 0xFFFFFFFF, kind, size))
                elif i.rs in tables:
                    out.append(("G", (tables[i.rs] + i.imm) & 0xFFFFFFFF, kind, size))
                elif i.rs != 29:
                    out.append((kind, size, (delta.get(i.rs, 0) + i.imm) & 0xFFFF))
                if kind == "L":
                    known.pop(i.rt, None)
                    entries.pop(i.rt, None)
                    delta.pop(i.rt, None)
                    if n == "lw" and i.rs in tables:
                        entries[i.rt] = (tables[i.rs] + i.imm) & 0xFFFFFFFF
                continue
            if n in ("jal", "j") and i.target is not None:
                out.append(("C", i.target))
                if n == "jal":
                    for r in (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31):
                        known.pop(r, None)
                continue
            if n in ("jalr", "jr") and i.rs != 31:
                if i.rs in entries:
                    out.append(("T", entries[i.rs]))
                else:
                    out.append(("C", known.get(i.rs)))
                continue
            for r in (i.rd, i.rt):  # anything else may write rd / rt
                if r:
                    known.pop(r, None)
                    tables.pop(r, None)
                    entries.pop(r, None)
                    delta.pop(r, None)
        return out

    def descriptors(self):
        """The 41 character descriptors (39 words: data / code pointers), in character order."""
        data, data_end = 0x1FEDE0, 0x1FEDE0 + 0xD6138
        found = []
        for a in range(data, data_end - DESC_WORDS * 4, 4):
            w = [self.u32(a + 4 * k) for k in range(DESC_WORDS)]
            code = sum(1 for k in CODE_WORDS if 0x1000 <= w[k] < self.text_end)
            dat = sum(1 for k in (12, 13, 14, 15, 28, 29, 30, 36, 37, 38) if data <= w[k] < data_end)
            if code + dat >= 21 and 0x1000 <= w[31] < self.text_end:
                if found and a - found[-1][0] < DESC_WORDS * 4:
                    continue
                found.append((a, w))
        return found


# ---------------- Dreamcast side ----------------

SH4_STORE = {"mov.b st": 1, "mov.w st": 2, "mov.l st": 4, "mov.b r0st": 1, "mov.w r0st": 2, "mov.l r0st": 4,
             "mov.b r0stdisp": 1, "mov.w r0stdisp": 2, "mov.l stdisp": 4}
SH4_LOAD = {"mov.b ld": 1, "mov.w ld": 2, "mov.l ld": 4, "mov.b r0ld": 1, "mov.w r0ld": 2, "mov.l r0ld": 4,
            "mov.b r0lddisp": 1, "mov.w r0lddisp": 2, "mov.l lddisp": 4,
            "mov.b ldinc": 1, "mov.w ldinc": 2, "mov.l ldinc": 4}


class Dc:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        self.img = Image(self.data, OVERLAY_BASE)
        entries = {OVERLAY_BASE}
        for off in range(0, len(self.data) - 3, 4):
            v = (struct.unpack_from("<I", self.data, off)[0] & 0x1FFFFFFF) | 0x80000000
            if self.img.contains(v) and not v & 1:
                entries.add(v)
        self.funcs = {a: f for a, f in discover(self.img, sorted(entries)).items() if not f.bad}

    def events(self, entry):
        return sh4_events(self.img, self.funcs.get(entry))

    def descriptor(self):
        return dc_descriptor(self)


class DcMain:
    """The Dreamcast main program (1ST_READ.BIN), explored a function at a time."""
    def __init__(self, path):
        self.img = Image(open(path, "rb").read(), 0x8C010000)
        self.cache = {}

    def events(self, entry):
        if entry not in self.cache:
            f = explore(self.img, entry, set()) if self.img.contains(entry) and not entry & 1 else None
            self.cache[entry] = sh4_events(self.img, f) if f and not f.bad else []
        return self.cache[entry]


def sh4_lit(img, a, size):
    if not img.contains(a):
        return None
    return img.u32(a) if size == 4 else struct.unpack_from("<h", img.data, a - img.base)[0] & 0xFFFFFFFF


def sh4_events(img, f):
    if not f:
        return []
    known, out, entries, delta = {}, [], {}, {}
    for a in sorted(f.insns):
        if a in f.labels:
            known, entries, delta = {}, {}, {}
        i = f.insns[a]
        n = i.name
        if n == "mov.l@pc":
            known[i.n] = sh4_lit(img, sh4.pcrel_addr(i, a), 4)
            continue
        if n == "mov.w@pc":
            known[i.n] = sh4_lit(img, sh4.pcrel_addr(i, a), 2)
            continue
        if n == "mov #imm":
            known[i.n] = (i.i - 0x100 if i.i & 0x80 else i.i) & 0xFFFFFFFF
            continue
        if n == "add #imm":
            imm = i.i - 0x100 if i.i & 0x80 else i.i
            if known.get(i.n) is not None:
                known[i.n] = (known[i.n] + imm) & 0xFFFFFFFF
            else:
                known.pop(i.n, None)
                if i.n != 15:
                    delta[i.n] = delta.get(i.n, 0) + imm
            continue
        if n == "add":  # add Rm,Rn
            c = known.get(i.m)
            if known.get(i.n) is None and isinstance(c, int) and c < 0x1000 and i.n != 15:
                delta[i.n] = delta.get(i.n, 0) + c
                entries.pop(i.n, None)
                continue
            known.pop(i.n, None)
            delta.pop(i.n, None)
            entries.pop(i.n, None)
            continue
        if n == "mov":
            if known.get(i.m) is not None:
                known[i.n] = known[i.m]
            else:
                known.pop(i.n, None)
            if i.m in delta:
                delta[i.n] = delta[i.m]
            else:
                delta.pop(i.n, None)
            entries.pop(i.n, None)
            continue
        if n in SH4_STORE or n in SH4_LOAD:
            size = SH4_STORE.get(n) or SH4_LOAD.get(n)
            kind = "S" if n in SH4_STORE else "L"
            if n.endswith("r0st") or n.endswith("r0ld"):
                base = i.n if kind == "S" else i.m
                off = known.get(0)
            elif n.endswith("disp"):
                base = i.n if kind == "S" else i.m
                off = i.d * size
            else:
                base = i.n if kind == "S" else i.m
                off = 0
            if base == 15:
                pass
            elif known.get(base) is not None and off is not None:
                out.append(("G", (known[base] + off) & 0xFFFFFFFF, kind, size))
            elif off is not None:
                out.append((kind, size, (delta.get(base, 0) + off) & 0xFFFF))
            elif known.get(base) is not None and (n.endswith("r0ld") or n.endswith("r0st")):
                out.append(("G", known[base], kind, size))  # table base + an index
            dst = 0 if n.endswith("r0lddisp") else i.n
            if kind == "L":
                known.pop(dst, None)
                entries.pop(dst, None)
                delta.pop(dst, None)
                if n == "mov.l r0ld" and known.get(i.m) is not None and known.get(0) is None:
                    entries[dst] = known[i.m]
            continue
        if n in ("jsr", "jmp") and i.n in entries:
            out.append(("T", entries[i.n]))
            if n == "jsr":
                for r in range(8):
                    known.pop(r, None)
            continue
        if n in ("jsr", "jmp"):
            out.append(("C", known.get(i.n)))
            if n == "jsr":
                for r in range(8):
                    known.pop(r, None)
            continue
        if n in ("bsr",):
            out.append(("C", sh4.branch_target(i, a)))
            continue
        if n == "bra":
            t = sh4.branch_target(i, a)
            if t not in f.insns:
                out.append(("C", t))
            continue
        known.pop(i.n, None)
        entries.pop(i.n, None)
        delta.pop(i.n, None)
        if n.endswith("r0lddisp"):
            known.pop(0, None)
    return out

def dc_descriptor(dc):
    """The character's descriptor: the 39 words the entry function copies (its mov.l @pc of a table)."""
    f = dc.funcs[OVERLAY_BASE]
    for a in sorted(f.insns):
        i = f.insns[a]
        if i.name == "mov.l@pc":
            v = sh4_lit(dc.img, sh4.pcrel_addr(i, a), 4)
            if v and dc.img.contains(v) and v + DESC_WORDS * 4 <= dc.img.end:
                w = [dc.img.u32(v + 4 * k) for k in range(DESC_WORDS)]
                if sum(1 for k in CODE_WORDS if dc.img.contains(w[k])) >= 10:
                    return w
    return None


# ---------------- alignment ----------------

PSP_DATA, PSP_DATA_END = 0x1FEDE0, 0x1FEDE0 + 0xD6138


def walk_tables(dc, psp, dw, pw, limit=4096):
    """Function pairs found by reading the descriptors' tables side by side: a
    table of pointers has the same shape on both machines, so a code pointer
    on one is the same function as the code pointer at that place on the other."""
    pairs, seen = [], set()
    todo = [(dw[k], pw[k]) for k in range(DESC_WORDS) if k not in CODE_WORDS]
    while todo and len(seen) < limit:
        da, pa = todo.pop()
        if (da, pa) in seen or not dc.img.contains(da) or not PSP_DATA <= pa < PSP_DATA_END:
            continue
        seen.add((da, pa))
        for k in range(256):
            a, b = da + 4 * k, pa + 4 * k
            if a + 4 > dc.img.end or b + 4 > PSP_DATA_END or (k and ((a, b) in seen)):
                break
            x, y = dc.img.u32(a), psp.u32(b)
            x_code = x in dc.funcs
            y_code = 0x1000 <= y < psp.text_end and not y & 3
            x_data = dc.img.contains(x) and not x_code
            y_data = PSP_DATA <= y < PSP_DATA_END
            if x_code and y_code:
                pairs.append((x, y))
            elif x_data and y_data:
                todo.append((x, y))
            elif (x == 0) != (y == 0) or (x_code or x_data) != (y_code or y_data):
                break  # the shapes differ: not a pointer table (any more)
    return pairs

def kinds(ev):
    return [(e[0], e[1]) if e[0] in "LS" else (e[0], e[3]) if e[0] == "G" else (e[0],) for e in ev]


class Matcher:
    """Pairs functions and lines up their events; every aligned pair votes."""
    def __init__(self, psp):
        self.psp = psp
        self.offs, self.globs, self.calls = collections.Counter(), collections.Counter(), collections.Counter()
        self.done = set()
        self.tables_done = set()
        self.dc_u32 = None

    def inlined(self, events, side_events, depth=1):
        """events with each call's own events spliced in (callees the compiler may have inlined)"""
        out = []
        for e in events:
            out.append(e)
            if e[0] == "C" and e[1] is not None and depth:
                sub = side_events(e[1])
                if 0 < len(sub) <= 40:
                    out.extend(sub)
        return out

    def vote(self, de, pe, local_dc=None):
        """align two event lists; returns callee pairs to follow"""
        follow = []
        sm = difflib.SequenceMatcher(None, kinds(de), kinds(pe), autojunk=False)
        for blk in sm.get_matching_blocks():
            for k in range(blk.size):
                x, y = de[blk.a + k], pe[blk.b + k]
                if x[0] in "LS":
                    self.offs[(x[1], x[2], y[2])] += 1
                elif x[0] == "G":
                    self.globs[(x[1], y[1])] += 1
                elif x[0] == "C" and x[1] is not None and y[1] is not None:
                    follow.append((x[1], y[1]))
                elif x[0] == "T":
                    follow += self.table_pairs(x[1], y[1])
        # calls alone keep their order well even where the rest drifts
        dcalls = [e for e in de if e[0] == "C" and e[1] is not None]
        pcalls = [e for e in pe if e[0] == "C" and e[1] is not None]
        if dcalls and pcalls:
            sm2 = difflib.SequenceMatcher(None, [1] * len(dcalls), [1] * len(pcalls), autojunk=False)
            if len(dcalls) == len(pcalls):
                follow += [(x[1], y[1]) for x, y in zip(dcalls, pcalls)]
        return follow, sm.ratio()

    def table_pairs(self, dt, pt):
        """two tables of function pointers read side by side"""
        out = []
        if (dt, pt) in self.tables_done or self.dc_u32 is None:
            return out
        self.tables_done.add((dt, pt))
        for k in range(512):
            x, y = self.dc_u32(dt + 4 * k), None
            if not PSP_DATA <= pt + 4 * k < PSP_DATA_END:
                break
            y = self.psp.u32(pt + 4 * k)
            xc = x is not None and 0x8C010000 <= x < 0x8D000000 and not x & 1
            yc = 0x1000 <= y < self.psp.text_end and not y & 3
            if xc and yc:
                out.append((x, y))
            elif x or y:
                break
        return out

    def pair(self, d, p, dc_events, psp_is_local=None):
        """align function d (Dreamcast) with p (PSP); returns follow-up pairs"""
        if (d, p) in self.done:
            return []
        self.done.add((d, p))
        de, pe = dc_events(d), self.psp.events(p)
        if not de or not pe:
            return []
        follow, r = self.vote(de, pe)
        if r < 0.7:  # perhaps a helper inlined on one side: try with callees spliced in
            de2 = self.inlined(de, dc_events)
            pe2 = self.inlined(pe, self.psp.events)
            best = max(((de2, pe), (de, pe2), (de2, pe2)),
                       key=lambda c: difflib.SequenceMatcher(None, kinds(c[0]), kinds(c[1]), autojunk=False).ratio())
            f2, r2 = self.vote(*best)
            if r2 > r:
                follow += f2
                r = r2
        if r < 0.45:
            return []
        return follow


def align(dc_dir, boot):
    psp = Psp(boot)
    descs = psp.descriptors()
    ids = [i for i in range(0x26) if i != 0x13]
    assert len(descs) >= len(ids), len(descs)
    m = Matcher(psp)
    main = DcMain(os.path.join(dc_dir, "1ST_READ.BIN"))
    engine_todo = []
    for cid, (pa, pw) in zip(ids, descs):
        path = os.path.join(dc_dir, "PL%02XA.BIN" % cid)
        dc = Dc(path)
        dw = dc.descriptor()
        if not dw:
            print("no descriptor in", path)
            continue
        todo = [(dw[k], pw[k]) for k in CODE_WORDS if dc.img.contains(dw[k]) and pw[k]]
        for x, y in [t for t in todo if main.img.contains(t[0])]:
            m.calls[(x, y)] += 1
            engine_todo.append((x, y))
        # engine functions named in the descriptor (word 31 and the like)
        for k in range(DESC_WORDS):
            if main.img.contains(dw[k]) and 0x1000 <= pw[k] < psp.text_end:
                m.calls[(dw[k], pw[k])] += 4
                engine_todo.append((dw[k], pw[k]))

        def dc_events(a, dc=dc):
            return dc.events(a) if dc.img.contains(a) else main.events(a)

        def dc_u32(a, dc=dc):
            img = dc.img if dc.img.contains(a) else main.img if main.img.contains(a) else None
            return img.u32(a) if img and img.contains(a + 3) else None
        m.dc_u32 = dc_u32
        while todo:
            d, p = todo.pop()
            if not dc.img.contains(d):
                continue
            for x, y in m.pair(d, p, dc_events):
                if dc.img.contains(x):
                    if x in dc.funcs:
                        todo.append((x, y))
                elif main.img.contains(x):
                    m.calls[(x, y)] += 1
                    engine_todo.append((x, y))
    # into the engine: paired engine functions pair the functions they call
    m.dc_u32 = lambda a: main.img.u32(a) if main.img.contains(a) and main.img.contains(a + 3) else None
    while engine_todo:
        d, p = engine_todo.pop()
        if not main.img.contains(d) or not 0x1000 <= p < psp.text_end or (d, p) in m.done:
            continue
        for x, y in m.pair(d, p, main.events):
            m.calls[(x, y)] += 1
            engine_todo.append((x, y))
    return m.offs, m.globs, m.calls, len(m.done)


def best(counter):
    """for each source key, its most-voted target, with votes and how many disagreed"""
    by = collections.defaultdict(collections.Counter)
    for (src, dst), n in counter.items():
        by[src][dst] += n
    out = {}
    for src, c in by.items():
        (dst, n), total = c.most_common(1)[0], sum(c.values())
        out[src] = (dst, n, total)
    return out


def export_maps(globs, calls):
    """PSP -> Dreamcast: each PSP function's most-voted counterpart, and the
    globals as segments of addresses that share one offset"""
    by = collections.defaultdict(collections.Counter)
    for (d, p), v in calls.items():
        if 0x8C010000 <= d < 0x8C800000:
            by[p][d] += v
    functions = {}
    for p, c in by.items():
        (d, v), total = c.most_common(1)[0], sum(c.values())
        if v >= 2 or total == 1:
            functions[hex(p)] = hex(d)
    gb = collections.defaultdict(collections.Counter)
    for (d, p), v in globs.items():
        if 0x8C010000 <= d < 0x8C800000 and not 0x1FEDE0 <= p < 0x2D4F30 - 0x1000:  # engine data, not tables in .data
            gb[p][d] += v
        elif 0x8C010000 <= d < 0x8C800000 and p >= 0x2D4F30:
            gb[p][d] += v
    pts = sorted((p, c.most_common(1)[0][0]) for p, c in gb.items() if c.most_common(1)[0][1] >= 2)
    segs = []
    for p, d in pts:
        if segs and d - p == segs[-1][2] - segs[-1][0] and p - segs[-1][1] <= 0x40:
            segs[-1][1] = p + 4
        else:
            segs.append([p, p + 4, d])
    return {"functions": functions, "globals": [[hex(a), hex(b), hex(c)] for a, b, c in segs]}


def main():
    dc_dir, boot = sys.argv[1], sys.argv[2]
    if len(sys.argv) > 4 and sys.argv[3] == "--maps":
        offs, globs, calls, n = align(dc_dir, boot)
        json.dump(export_maps(globs, calls), open(sys.argv[4], "w"), indent=0)
        print("function pairs aligned:", n)
        return
    offs, globs, calls, n = align(dc_dir, boot)
    print("function pairs aligned:", n)
    om = best({((s, a), b): v for (s, a, b), v in offs.items()})
    gm, cm = best(globs), best(calls)
    print("struct offsets:", len(om), " globals:", len(gm), " engine functions:", len(cm))
    if len(sys.argv) > 3:
        json.dump({"offsets": {f"{s}:{a}": v for (s, a), v in om.items()},
                   "globals": {hex(k): [hex(v[0]), v[1], v[2]] for k, v in gm.items()},
                   "functions": {hex(k): [hex(v[0]), v[1], v[2]] for k, v in cm.items()}},
                  open(sys.argv[3], "w"), indent=1)
    return om, gm, cm


if __name__ == "__main__":
    main()
