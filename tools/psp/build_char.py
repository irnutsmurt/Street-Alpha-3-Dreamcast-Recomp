"""Build PSP characters for the Dreamcast game: recompiled code, and character
files (PLnnA/B/C.BIN) in the Dreamcast's form.

    python3 tools/psp/build_char.py <PSP BOOT.BIN> <PSP data dir> <Dreamcast files dir> <out dir> <maps.json>
                                    <character>=<file id>[,<character>=<file id>...]

character: the PSP's character number (0x00-0x29); file id: the Dreamcast
file number to write (PLnnA/B/C.BIN). The maps come from align.py.

A character file holds, from its start:
  0x000  the entry stub (the engine calls the file's first address)
  0x008  a stub for every function the engine may call (8 bytes: FFFF 5053 <index> FFFF)
         then the PSP data the code uses that has no Dreamcast counterpart,
         the character's own data (descriptor and tables, pointers turned
         into Dreamcast ones) and its animation data (the PSP keeps it in
         the PAC's section 0x11, the Dreamcast in this file).
"""
import bisect
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import chartables  # noqa: E402
import mips  # noqa: E402
from mipsgen import Boot, Function, Gen, R_MIPS_HI16, R_MIPS_LO16, TEXT_END  # noqa: E402

SLOT_BASE, SLOT_SIZE = 0x8C800000, 0x14000
DATA, DATA_END = 0x1FEDE0, 0x1FEDE0 + 0xD6138
DESC_WORDS = 39
CODE_WORDS = range(16, 28)
HELPER_LIB = (0xCE000, 0xD1000)      # code only the PSP-only characters use
BUILDER = 0x6AB48                    # builds descriptor words 0-11 / 32-34 at load: done here instead
BUILDERS = {BUILDER, 0x1BF93C}       # and Yun's own builder: no-ops at run time (run_entry did their work)
LAST_CODE_END = 0x1CB124             # Ingrid's code ends here (a PSP-only module follows)
# engine code the Dreamcast's engine runs for these characters (runtime/psp.c): none so far (the
# CPU script command the PSP added is run through the Dreamcast's: hle_ai_step)
ENGINE_ROOTS = ()
LAST_DATA_END = 0x2B2310             # and her data (the module's tables follow)
BSS, BSS_END = 0x2D4FC0, 0x2D4FC0 + 0xA4D454
EXT_PSP_VARS, EXT_PSP_VARS_END = chartables.EXT_END, 0x98F00000  # PSP-only variables (extension RAM, zeroed at boot)
PSP_FIGHTERS, PSP_FIGHTERS_END = 0x80AF70, 0x80AF70 + 4 * 0x498
PSP_POOLS, PSP_POOLS_END = 0xA39840, 0xA3E7C0
PSP_SHOTS = 0xA37080                  # projectiles, then the effect pools
PALETTES = (0x2C7F00, 0x2C8500)          # effect palettes the PSP shares between characters


def descriptors(boot):
    """the PSP's character descriptors, in character order (0x13 has none)"""
    found = []
    for a in range(DATA, DATA_END - DESC_WORDS * 4, 4):
        w = struct.unpack_from("<%dI" % DESC_WORDS, boot.data, a + 0x74)
        code = sum(1 for k in CODE_WORDS if 0x1000 <= w[k] < TEXT_END)
        dat = sum(1 for k in (12, 13, 14, 15, 28, 29, 30, 36, 37, 38) if DATA <= w[k] < DATA_END)
        if code + dat >= 21 and 0x1000 <= w[31] < TEXT_END:
            if found and a - found[-1][0] < DESC_WORDS * 4:
                continue
            found.append((a, list(w)))
    ids = [i for i in range(0x26) if i != 0x13] + [0x26, 0x27, 0x28, 0x29]
    return dict(zip(ids, found))


def pac_sections(path):
    d = open(path, "rb").read()
    n = struct.unpack_from("<I", d, 0)[0]
    out, off = {}, 0x800
    for k in range(n):
        t, s = struct.unpack_from("<II", d, 8 + 8 * k)
        out[t] = d[off:off + s]
        off = (off + s + 0x7FF) & ~0x7FF
    return out


class Character:
    def __init__(self, boot, cid, descs, code_ranges):
        self.boot, self.cid = boot, cid
        self.desc, self.dw = descs[cid]
        self.code = code_ranges[cid]
        self.entry = self.find_entry()
        if self.entry < self.code[0]:
            self.code = (self.entry, self.code[1])
        lo, hi = self.reach()
        # up to the next character's data
        nxt = [c for c in descs if descs[c][0] > self.desc]
        if nxt:
            n = min(nxt, key=lambda c: descs[c][0])
            other = Character.__new__(Character)
            other.boot, other.cid, (other.desc, other.dw), other.code = boot, n, descs[n], code_ranges[n]
            hi = min(hi, other.reach()[0])
        else:
            hi = min(hi, LAST_DATA_END)
        self.own = (lo, hi)

    def reach(self):
        """the data the character's code and descriptor reach, near the descriptor"""
        b = self.boot
        lo_win, hi_win = self.desc - 0x9000, self.desc + 0x3000
        seeds = {self.desc} | {a for a in self.code_refs() if lo_win <= a < hi_win}
        seen, todo = set(), list(seeds)
        while todo:
            a = todo.pop()
            if a in seen or not lo_win <= a < hi_win:
                continue
            seen.add(a)
            for k in range(0, 0x200, 4):
                if a + k in b.data_ptrs:
                    v = b.u32(a + k)
                    if DATA <= v < DATA_END:
                        todo.append(v)
        lo, hi = min(seen), max(seen) + 0x200
        return lo & ~3, (hi + 3) & ~3

    def code_refs(self):
        """addresses built by HI16/LO16 pairs in the character's code"""
        b, out, his = self.boot, set(), {}
        lo, hi = self.code
        for a in range(lo, hi, 4):
            rel = b.text_rel.get(a)
            i = b.insn(a)
            if rel == R_MIPS_HI16:
                his[i.rt] = i.imm << 16
            elif rel == R_MIPS_LO16 and i.rs in his:
                out.add((his[i.rs] + i.imm) & 0xFFFFFFFF)
        return out

    def find_entry(self):
        """the function that copies the descriptor (it builds the descriptor's address)"""
        b = self.boot
        lo, hi = self.code
        lo -= 0x400  # the entry function may sit just before the rest
        his = {}
        for a in range(lo, hi, 4):
            rel = b.text_rel.get(a)
            i = b.insn(a)
            if rel == R_MIPS_HI16:
                his[i.rt] = i.imm << 16
            elif rel == R_MIPS_LO16 and i.rs in his and (his[i.rs] + i.imm) & 0xFFFFFFFF == self.desc:
                # back to the function's start: the stack frame setup
                p = a
                while p > lo and not (b.insn(p).name == "addiu" and b.insn(p).rt == 29 and b.insn(p).rs == 29
                                      and b.insn(p).imm < 0):
                    p -= 4
                return p
        raise SystemExit(f"character {self.cid:02X}: no entry function found")

    def builder_table(self):
        """the index bytes the entry function hands the PSP's table builder (its a2)"""
        b, f = self.boot, Function(self.boot, self.entry)
        his, regs = {}, {}
        for a in f.addrs:
            i = b.insn(a)
            rel = b.text_rel.get(a)
            if rel == R_MIPS_HI16:
                his[i.rt] = i.imm << 16
            elif rel == R_MIPS_LO16 and i.rs in his:
                regs[i.rt] = (his[i.rs] + i.imm) & 0xFFFFFFFF
            if i.name == "jal" and i.target == BUILDER:
                d = b.insn(a + 4)  # the delay slot may set a2 too
                if b.text_rel.get(a + 4) == R_MIPS_LO16 and d.rs in his:
                    regs[d.rt] = (his[d.rs] + d.imm) & 0xFFFFFFFF
                return regs[6]
        raise SystemExit(f"character {self.cid:02X}: the entry function doesn't call the table builder")

    def function_pointers(self):
        """code addresses in the character's own data"""
        b, lo, hi = self.boot, *self.own
        return {b.u32(a) for a in range(lo, hi, 4) if a in b.data_ptrs and 0x1000 <= b.u32(a) < TEXT_END}


class Build:
    def __init__(self, boot_path, psp_data, maps):
        self.boot = Boot(boot_path)
        self.psp_data = psp_data
        mp = json.load(open(maps))
        self.fmap = {int(k, 16): int(v, 16) for k, v in mp["functions"].items()}
        self.gsegs = [(int(a, 16), int(b, 16), int(c, 16)) for a, b, c in mp["globals"]]
        manual = json.load(open(os.path.join(HERE, "manual_maps.json")))
        for g in manual["globals"]:  # checked by hand: these win over the voted ones
            lo, hi, dc = (int(x, 16) for x in g[:3])
            self.gsegs = [s for s in self.gsegs if s[1] <= lo or s[0] >= hi] + [(lo, hi, dc)]
        self.fmap.update({int(k, 16): int(v, 16) for k, v in manual["functions"].items()})
        # the character tables: the moved copies have the PSP's 42-entry layout
        for t in chartables.layout():
            if t.psp is None:
                continue
            lo, hi = t.psp, t.psp + t.entries * t.size
            self.gsegs = [s for s in self.gsegs if s[1] <= lo or s[0] >= hi] + [(lo, hi, t.ext)]
        self.gsegs.sort()
        self.descs = descriptors(self.boot)
        order = sorted(self.descs)
        starts = {c: min(w[k] for k in CODE_WORDS if w[k]) for c, (a, w) in self.descs.items()}
        srt = sorted(starts.items(), key=lambda x: x[1])
        self.code_ranges = {}
        for k, (c, s) in enumerate(srt):
            self.code_ranges[c] = (s, srt[k + 1][1] if k + 1 < len(srt) else LAST_CODE_END)
        self.chars = []
        self._refs = None
        self.copies = []      # PSP data with no Dreamcast counterpart, copied into the character files
        self.vars_next = EXT_PSP_VARS
        self.funcs = {}       # PSP address -> Function
        self.order = []
        self.taken = []       # functions given stubs, in stub order
        self.problems = []

    def is_char_code(self, a):
        return any(lo <= a < hi for lo, hi in (c.code for c in self.chars)) or HELPER_LIB[0] <= a < HELPER_LIB[1]

    def resolve(self, t):
        if t in BUILDERS:
            return ("nop",)
        if t in self.funcs:
            return ("psp", f"p_{t:06X}")
        if t in self.fmap and not self.is_char_code(t):
            return ("dc", self.fmap[t])
        return None

    def add_char(self, cid):
        c = Character(self.boot, cid, self.descs, self.code_ranges)
        self.chars.append(c)
        return c

    def all_refs(self):
        """every data address the PSP program refers to: built in its code (HI16/LO16) or kept in its data"""
        if self._refs is None:
            refs, his = set(), {}
            for a in sorted(self.boot.text_rel):
                rel = self.boot.text_rel[a]
                if rel == R_MIPS_HI16:
                    i = self.boot.insn(a)
                    his[i.rt] = i.imm << 16
                elif rel == R_MIPS_LO16:
                    i = self.boot.insn(a)
                    if i.rs in his:
                        refs.add((his[i.rs] + i.imm) & 0xFFFFFFFF)
            refs |= {self.boot.u32(p) for p in self.boot.data_ptrs}
            self._refs = sorted(r for r in refs if DATA <= r < BSS_END)
        return self._refs

    def extent(self, a):
        """a data object: from a up to the next address the program refers to"""
        refs = self.all_refs()
        i = bisect.bisect_right(refs, a)
        return min(refs[i] if i < len(refs) else a + 16, a + 0x8000)

    def collect(self):
        """the functions to recompile (the characters' code and whatever it reaches with no Dreamcast
        counterpart) and the PSP data they need with none: copied whole, the pointers in it followed"""
        todo, data = [], []  # data: (address, how many pointers away from the code)
        for c in self.chars:
            todo += [c.dw[k] for k in CODE_WORDS if c.dw[k]] + [c.entry] + sorted(c.function_pointers())
            todo += [a for a in c.code_refs() if 0x1000 <= a < TEXT_END]
            lo, hi = c.own
            data += [(self.boot.u32(a), 0) for a in range(lo, hi, 4) if a in self.boot.data_ptrs]
        todo += list(ENGINE_ROOTS)
        taken = set(todo)
        engine_recompiled = set()
        seen = set()
        self.psp_vars = []
        while todo or data:
            if not todo:
                a, depth = data.pop()
                if a in seen or self.global_dc(a) is not None or self.in_own(a) or not DATA <= a < BSS_END:
                    continue
                if PSP_FIGHTERS <= a < PSP_FIGHTERS_END or PSP_SHOTS <= a < PSP_POOLS_END:
                    continue  # the object arrays: translated as the code runs (psp.c)
                seen.add(a)
                hi = self.extent(a)
                if a >= BSS:  # a variable the Dreamcast doesn't have: room in the extension RAM
                    size = (hi - a + 3) & ~3
                    self.gsegs.append((a, hi, self.vars_next))
                    self.gsegs.sort()
                    self.psp_vars.append((a, hi, self.vars_next))
                    self.vars_next += size
                    if self.vars_next > EXT_PSP_VARS_END:
                        raise SystemExit("PSP-only variables outgrew their room: " +
                                         " ".join(f"{lo:06X}+{h - lo:X}" for lo, h, _ in self.psp_vars))
                    continue
                self.copies.append((a, hi))
                if depth >= 2:  # far from the code: its pointers aren't followed (they're reported if unresolved)
                    continue
                for p in range(a & ~3, hi, 4):
                    if p in self.boot.data_ptrs:
                        v = self.boot.u32(p)
                        if 0x1000 <= v < TEXT_END:
                            # a character's code; or engine code with no counterpart in a table the
                            # recompiled code reads directly (a handler table); else its counterpart
                            if self.is_char_code(v) or (depth == 0 and v not in self.fmap):
                                todo.append(v)
                                taken.add(v)
                        else:
                            data.append((v, depth + 1))
                continue
            a = todo.pop()
            if a in self.funcs or a in BUILDERS:
                continue
            if not self.is_char_code(a) and a in self.fmap:
                continue
            if not self.is_char_code(a):
                engine_recompiled.add(a)
            f = Function(self.boot, a)
            self.funcs[a] = f
            switch_tables = {jt[1] for jt in f.tables.values()}  # the recompiled code has these built in
            his = {}
            for x in f.addrs:
                i = self.boot.insn(x)
                rel = self.boot.text_rel.get(x)
                if i.name in ("jal", "j") and i.target is not None and i.target not in f.addrs:
                    todo.append(i.target)
                if rel == R_MIPS_HI16:
                    his[i.rt] = i.imm << 16
                elif rel == R_MIPS_LO16 and i.rs in his:
                    v = (his[i.rs] + i.imm) & 0xFFFFFFFF
                    if 0x1000 <= v < TEXT_END:
                        if v not in f.addrs:
                            todo.append(v)
                            taken.add(v)
                    elif v not in switch_tables:
                        data.append((v, 0))
        self.order = sorted(self.funcs)
        self.taken = sorted(a for a in taken if a in self.funcs)
        self.engine_recompiled = sorted(engine_recompiled)

    # ---- address translation (build time: data pointers) ----
    def global_dc(self, a):
        for lo, hi, dc in self.gsegs:
            if lo <= a < hi:
                return dc + (a - lo)
        return None

    def layout(self):
        """offsets of everything in a character file"""
        self.stub_off = {a: 8 + 8 * k for k, a in enumerate(self.taken)}
        self.shared_off = {}
        off = 8 + 8 * len(self.taken)
        # the PSP data copied (collect()), on 4-byte boundaries
        ranges = [[lo & ~3, (hi + 3) & ~3] for lo, hi in self.copies]
        # the shared palettes are copied whole
        ranges = [[min(lo, PALETTES[0]) if PALETTES[0] <= lo < PALETTES[1] else lo,
                   max(hi, PALETTES[1]) if PALETTES[0] <= lo < PALETTES[1] else hi] for lo, hi in ranges]
        merged = []
        for lo, hi in sorted(ranges):
            if merged and lo <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], hi)
            else:
                merged.append([lo, hi])
        ranges = merged
        self.shared = []
        off = (off + 15) & ~15
        for lo, hi in ranges:
            self.shared.append((lo, hi, off))
            off += hi - lo
        self.common_end = off

    def in_own(self, a):
        return any(c.own[0] <= a < c.own[1] for c in self.chars)

    def data_refs(self):
        """data addresses built by the recompiled code"""
        out = set()
        for fa in self.order:
            f = self.funcs[fa]
            his = {}
            for x in f.addrs:
                i = self.boot.insn(x)
                rel = self.boot.text_rel.get(x)
                if rel == R_MIPS_HI16:
                    his[i.rt] = i.imm << 16
                elif rel == R_MIPS_LO16 and i.rs in his:
                    out.add((his[i.rs] + i.imm) & 0xFFFFFFFF)
        return out


# ---------------- the descriptor tables the PSP builds at load (0x6AB48 / 0x6A638) ----------------

# (index byte, D offset) in the order the PSP's builder writes them
BUILDER_SLOTS = [(0, 0xC8), (1, 0x308), (2, 0x548), (3, 0x788), (4, 0x9D4), (5, 0x9D8), (6, 0x9DC), (7, 0x9E0),
                 (8, 0x9E4), (9, 0x9E8), (10, 0x9EC), (11, 0x9F0), (12, 0x9FC), (13, 0x10), (14, 0x18), (15, 0x20),
                 (16, 0x28), (17, 0x2C), (18, 0x80), (19, 0x84), (20, 0x88)]
D_SIZE = 0xA08


def pool_words(sec, idx):
    """what 0x6A638 writes for entry idx of section 0x11: ('A', offset) values"""
    A = 0x208
    off = struct.unpack_from("<I", sec, 8 + idx * 4)[0]
    cnt = struct.unpack_from("<i", sec, 0x108 + idx * 4)[0]
    if cnt == 0:
        return [("A", off)]
    if cnt == -1:
        out, p = [], A + off
        while True:
            v = struct.unpack_from("<i", sec, p)[0]
            if v == -1:
                return out
            out.append(("A", 10 * v))
            p += 4
    if cnt > 0:
        out, p = [], A + off
        for k in range(cnt):
            v = struct.unpack_from("<I", sec, p)[0]
            out.append(("A", struct.unpack_from("<I", sec, 8 + 4 * (idx + v + 1))[0]))
            p += 4
        return out
    return []


def build_d(sec, index_bytes):
    """the descriptor RAM block as the PSP's builder leaves it: D offset -> value
    (('A', off) into section 0x11's data, ('D', off) into D itself)"""
    d = {}
    for k, doff in BUILDER_SLOTS:
        idx = struct.unpack_from("<b", index_bytes, k)[0]
        for n, v in enumerate(pool_words(sec, idx)):
            d[doff + 4 * n] = v
    for k in range(3):
        d[0x9C8 + 4 * k] = ("D", 0x9D4)
    d[0x14] = ("D", 0x9C8)
    d[0x0], d[0x4], d[0x8], d[0xC] = ("D", 0xC8), ("D", 0x308), ("D", 0x548), ("D", 0x788)
    d[0x1C], d[0x24] = ("D", 0x9F0), ("D", 0x9FC)
    return d


FIGHTER, DBLOCK, POOL, STACK = 0x0B000000, 0x0A000000, 0x0C000000, 0x0F000000


def run_entry(b, c, sec11):
    """run the character's entry function (MIPS, interpreted) against scratch
    memory, and read back the descriptor block it leaves: D offset -> value,
    ('A', off) into section 0x11's data, ('D', off) into the block itself,
    ('P', address) PSP data (a pointer the file's relocation turns into ours)"""
    from mipsemu import Cpu, Memory
    mem = Memory(b.boot)
    fighter = bytearray(0x600)
    struct.pack_into("<I", fighter, 0x1A0, DBLOCK)
    dblock = bytearray(D_SIZE + 0x400)
    struct.pack_into("<II", dblock, 0xC0, POOL, POOL + 0x208)  # where the PSP keeps section 0x11
    mem.add(FIGHTER, fighter)
    mem.add(DBLOCK, dblock)
    mem.add(POOL, sec11)
    mem.add(STACK - 0x10000, bytearray(0x11000))
    lo, hi = c.code

    def allow(t):  # the character's own code and the PSP's table builder
        return lo <= t < hi or t in (BUILDER, 0x6A638)
    cpu = Cpu(mem, allow)
    cpu.call(c.entry, (FIGHTER, 0, 0, 0))
    d = {}
    for off in range(0, len(dblock) - 3, 4):
        v = struct.unpack_from("<I", dblock, off)[0]
        if off in (0xC0, 0xC4) or not any(DBLOCK + off + k in mem.writes for k in range(4)):
            continue
        if POOL + 0x208 <= v < POOL + len(sec11):
            d[off] = ("A", v - POOL - 0x208)
        elif DBLOCK <= v < DBLOCK + len(dblock):
            d[off] = ("D", v - DBLOCK)
        elif v == 0:
            d[off] = ("Z", 0)
        else:
            d[off] = ("P", v)
    return d


# ---------------- writing ----------------

def stub(index):
    return struct.pack("<HHHH", 0xFFFF, 0x5053, index, 0xFFFF)


class Writer:
    def __init__(self, build, c, sec11, text=None):
        self.b, self.c, self.sec = build, c, sec11
        b = build
        self.own_off = b.common_end
        lo, hi = c.own
        self.sec_off = (self.own_off + (hi - lo) + 15) & ~15
        self.d_off = (self.sec_off + len(sec11) - 0x208 + 15) & ~15
        self.d = run_entry(b, c, sec11)
        self.size = self.d_off + max(D_SIZE, max(self.d) + 4)
        self.text, self.text_off = None, self.size
        if text is not None:
            self.text = text
            self.size += 4 * text[0] + sum(len(r) for r in text[1])
        if self.size > SLOT_SIZE:
            raise SystemExit(f"character {c.cid:02X}: {self.size} bytes, more than a slot holds")

    def xa(self, v, slot):
        """a PSP pointer in data -> Dreamcast, for the file in this slot"""
        b, c = self.b, self.c
        base = SLOT_BASE + slot * SLOT_SIZE
        lo, hi = c.own
        if lo <= v < hi:
            return base + self.own_off + (v - lo)
        for slo, shi, off in b.shared:
            if slo <= v < shi:
                return base + off + (v - slo)
        if 0x1000 <= v < TEXT_END:
            if v in b.stub_off:
                return base + b.stub_off[v]
            if v in b.fmap and not b.is_char_code(v):
                return b.fmap[v]
            b.problems.append(f"{c.cid:02X}: code pointer {v:06X} with no stub or counterpart")
            return 0
        g = b.global_dc(v)
        if g is not None:
            return g
        if PSP_FIGHTERS <= v < PSP_FIGHTERS_END and (v - PSP_FIGHTERS) % 0x498 == 0:
            return 0x8C17FD38 + (v - PSP_FIGHTERS) // 0x498 * 0x44C  # a fighter
        b.problems.append(f"{c.cid:02X}: data pointer {v:06X} with no counterpart")
        return 0

    def text_for(self, slot):
        """the dialogue table: (count) pointers then the records; pointers made for this slot"""
        n, recs = self.text
        base = SLOT_BASE + slot * SLOT_SIZE + self.text_off
        out = bytearray()
        pos = 4 * n
        for r in recs:
            out += struct.pack("<I", base + pos)
            pos += len(r)
        for r in recs:
            out += r
        return bytes(out)

    def dval(self, v, slot):
        base = SLOT_BASE + slot * SLOT_SIZE
        kind, off = v
        if kind == "A":
            return base + self.sec_off + off
        if kind == "D":
            return base + self.d_off + off
        if kind == "Z":
            return 0
        return self.xa(off, slot)  # PSP data: the descriptor's own words

    def file(self, slot):
        b, c = self.b, self.c
        out = bytearray(self.size)
        fidx = {a: k for k, a in enumerate(b.order)}
        out[0:8] = stub(fidx[c.entry])
        for a, off in b.stub_off.items():
            out[off:off + 8] = stub(fidx[a])

        def copy(lo, hi, off, palette=False):
            out[off:off + hi - lo] = b.boot.data[lo + 0x74:hi + 0x74]
            if palette:  # PSP colours are ABGR1555, the Dreamcast's ARGB1555
                for k in range(off, off + hi - lo - 1, 2):
                    v = struct.unpack_from("<H", out, k)[0]
                    struct.pack_into("<H", out, k, (v & 0x83E0) | ((v & 0x1F) << 10) | ((v >> 10) & 0x1F))
            for a in range(lo, hi, 4):
                if a in b.boot.data_ptrs:
                    struct.pack_into("<I", out, off + a - lo, self.xa(b.boot.u32(a), slot))
        for lo, hi, off in b.shared:
            copy(lo, hi, off, palette=PALETTES[0] <= lo < PALETTES[1])
        lo, hi = c.own
        copy(lo, hi, self.own_off)
        # the descriptor as the PSP's builder would leave it
        dpos = self.own_off + (c.desc - lo)
        for doff, v in self.d.items():
            if doff < DESC_WORDS * 4:
                struct.pack_into("<I", out, dpos + doff, self.dval(v, slot))
        if self.text is not None:  # word 38's entry 1: the dialogue table
            w38 = self.own_off + (c.dw[38] - lo)
            struct.pack_into("<I", out, w38 + 4, SLOT_BASE + slot * SLOT_SIZE + self.text_off)
            out[self.text_off:self.text_off + len(self.text)] = self.text_for(slot)
        out[self.sec_off:self.sec_off + len(self.sec) - 0x208] = self.sec[0x208:]
        for doff, v in self.d.items():
            if doff >= DESC_WORDS * 4:
                struct.pack_into("<I", out, self.d_off + doff, self.dval(v, slot))
        return bytes(out)


def char_data(patches):
    """the add-on's character table entries as g_psp_char_data (runtime/chartables.h)"""
    blob = bytearray()
    for a, v in patches:
        blob += struct.pack("<II", a, len(v)) + v + bytes(-len(v) & 3)
    rows = [", ".join(f"0x{x:02X}" for x in blob[i:i + 24]) for i in range(0, len(blob), 24)]
    return ("const uint8_t g_psp_char_data[] = {\n" + "".join(f"    {r},\n" for r in rows) + "    0};\n"
            f"const unsigned g_psp_char_data_size = {len(blob)};\n")


def emit_c(b, out_dir, patches=(), screens=None):
    gen = Gen(b.boot, b.resolve)
    bodies = [gen.function(b.funcs[a], f"p_{a:06X}") for a in b.order]
    head = ['/* Generated by tools/psp/build_char.py from the PSP game: do not edit. */',
            '#include <math.h>', '#include "chartables.h"', '#include "psp.h"', ""]
    protos = [f"void p_{a:06X}(void);" for a in b.order]
    chunks, cur, n = [], [], 0
    for body in bodies:
        cur.append(body)
        n += len(body)
        if n > 400000:
            chunks.append(cur)
            cur, n = [], 0
    if cur:
        chunks.append(cur)
    tables = getattr(gen, "table_defs", [])
    for k, ch in enumerate(chunks):
        with open(os.path.join(out_dir, f"psp_code_{k}.c"), "w") as f:
            f.write("\n".join(head + protos + [""] + tables + [""] + ch) + "\n")
    regions = [(PSP_FIGHTERS, PSP_FIGHTERS_END, "PSP_R_FIGHTERS", 0), (PSP_SHOTS, PSP_POOLS_END, "PSP_R_POOLS", 0)]
    regions += [(lo, hi, "PSP_R_LINEAR", dc) for lo, hi, dc in b.gsegs]
    regions += [(lo, hi, "PSP_R_SLOT", off) for lo, hi, off in b.shared]
    regions += [(c.own[0], c.own[1], "PSP_R_SLOT", b.common_end) for c in b.chars]
    regions.sort()
    code = {a: off | 1 for a, off in b.stub_off.items()}
    for p, d in b.fmap.items():
        if p not in code and not b.is_char_code(p):
            code[p] = d
    with open(os.path.join(out_dir, "psp_tables.c"), "w") as f:
        f.write("\n".join(head + protos + [""]))
        f.write("\nconst PspFn g_psp_fns[] = {\n" + "".join(f"    p_{a:06X},\n" for a in b.order) + "};\n")
        f.write(f"const unsigned g_psp_fn_count = {len(b.order)};\n")
        f.write("const uint32_t g_psp_fn_addr[] = {" + ", ".join(f"0x{a:06X}u" for a in b.order) + "};\n")
        f.write("const PspRegion g_psp_regions[] = {\n" +
                "".join(f"    {{0x{lo:06X}u, 0x{hi:06X}u, {k}, 0x{base:08X}u}},\n" for lo, hi, k, base in regions) + "};\n")
        f.write(f"const unsigned g_psp_region_count = {len(regions)};\n")
        f.write("const PspCode g_psp_code[] = {\n" +
                "".join(f"    {{0x{p:06X}u, 0x{code[p]:08X}u}},\n" for p in sorted(code)) + "};\n")
        f.write(f"const unsigned g_psp_code_count = {len(code)};\n")
        f.write(char_data(patches))
        screens = screens or dict(portrait_palettes=0, layout_extras=(), vs_palette_shift=0)
        f.write(f"const uint32_t g_psp_portrait_palettes = {screens['portrait_palettes']};\n")
        rows = "".join(f"    {{{pose}u, 0x{a:08X}u}},\n" for pose, a in screens["layout_extras"])
        f.write("const uint32_t g_psp_layout_extras[][2] = {\n" + rows + "    {0, 0}};\n")
        f.write(f"const unsigned g_psp_layout_extra_count = {len(screens['layout_extras'])};\n")
        f.write(f"const uint32_t g_psp_vs_palette_shift = {screens['vs_palette_shift']};\n")
        rows = ", ".join("{" + ", ".join(f"0x{v:X}u" for v in four) + "}" for four in screens.get("hud_parts", ()))
        f.write(f"const uint16_t g_psp_hud_parts[4][4] = {{{rows or '{0}'}}};\n")
    return gen.problems, len(chunks)


def dc_text(dc_dir, cid, psp_data=None):
    """the Dreamcast character's pre-fight dialogue (word 38, entry 1); for a
    PSP character the PSP's (its bd file's section 0x10001: 128-byte blocks,
    two lines each); or empty lines"""
    path = os.path.join(dc_dir, f"PL{cid:02X}A.BIN")
    bd = psp_data and os.path.join(psp_data, f"bd{cid:02x}_u.pac")
    if not os.path.exists(path) and bd and os.path.exists(bd):
        sec = pac_sections(bd)[0x10001]
        entries = []
        for k in range(0, len(sec), 128):
            block = sec[k:k + 128]
            first = block.index(0)
            entries.append(block[:block.index(0, first + 1) + 1])
        return len(entries), entries
    if os.path.exists(path):
        sys.path.insert(0, os.path.join(HERE, "..", "recomp"))
        from align import Dc
        dc = Dc(path)
        dw = dc.descriptor()
        t = struct.unpack_from("<I", dc.data, dw[38] - SLOT_BASE + 4)[0] - SLOT_BASE
        ptrs = []
        while len(ptrs) < 64:
            v = struct.unpack_from("<I", dc.data, t + 4 * len(ptrs))[0]
            if not SLOT_BASE <= v < SLOT_BASE + len(dc.data) or (ptrs and v < ptrs[-1]):
                break
            ptrs.append(v)
        ends = ptrs[1:] + [min(v for v in dw if isinstance(v, int) and v > ptrs[-1] and SLOT_BASE <= v < SLOT_BASE + len(dc.data)) if ptrs else 0]
        return len(ptrs), [dc.data[a - SLOT_BASE:e - SLOT_BASE] for a, e in zip(ptrs, ends)]
    return 20, [b"\x00\x00\x00\x00"] * 20


def main():
    boot_path, psp_data, dc_dir, out_dir, maps, spec = sys.argv[1:7]
    b = Build(boot_path, psp_data, maps)
    pairs = [(int(x, 16), int(y, 16)) for x, y in (p.split("=") for p in spec.split(","))]
    for cid, _ in pairs:
        b.add_char(cid)
    b.collect()
    b.layout()
    print(f"functions to recompile: {len(b.funcs)} (stubs {len(b.taken)})")
    print("PSP-only variables (extension RAM):", " ".join(f"{lo:06X}+{hi - lo:X}" for lo, hi, _ in b.psp_vars))
    print("PSP data copied:", " ".join(f"{lo:06X}+{hi - lo:X}" for lo, hi in sorted(b.copies)))
    print("engine functions recompiled from the PSP:", " ".join(f"{a:06X}" for a in b.engine_recompiled))
    os.makedirs(out_dir, exist_ok=True)
    for (cid, fid), c in zip(pairs, b.chars):
        sec11 = pac_sections(os.path.join(psp_data, f"pl{cid:02x}_u.pac"))[0x11]
        w = Writer(b, c, sec11, dc_text(dc_dir, cid, psp_data))
        for slot, letter in enumerate("ABC"):
            open(os.path.join(out_dir, f"PL{fid:02X}{letter}.BIN"), "wb").write(w.file(slot))
        json.dump({"own": w.own_off, "own_psp": c.own, "desc_psp": c.desc, "sec": w.sec_off, "d": w.d_off,
                   "text": w.text_off, "shared": b.shared, "stubs": len(b.taken)},
                  open(os.path.join(out_dir, f"PL{fid:02X}.layout.json"), "w"))
        print(f"character {c.cid:02X} -> PL{fid:02X}A/B/C.BIN: entry {c.entry:06X}, code {c.code[0]:06X}-{c.code[1]:06X}, "
              f"own data {c.own[0]:06X}-{c.own[1]:06X}, {w.size} bytes")
    patches = chartables.patches(chartables.Programs(os.path.join(dc_dir, "1ST_READ.BIN"), b.boot.data))
    print(f"character tables: {len(patches)} entries and records for the new characters")
    import screens
    sel, sel_patches, screen_info = screens.build(
        open(os.path.join(dc_dir, "SELECT.PAC"), "rb").read(), open(os.path.join(psp_data, "select_u.pac"), "rb").read(),
        open(os.path.join(dc_dir, "1ST_READ.BIN"), "rb").read(), b.boot.data)
    open(os.path.join(out_dir, "SELECT.PAC"), "wb").write(sel)
    patches += sel_patches
    print(f"select screen: SELECT.PAC {len(sel)} bytes, {len(sel_patches)} patches")
    import hud
    pitu, hud_patches, hud_parts = hud.build(
        open(os.path.join(dc_dir, "PITU.PAC"), "rb").read(), open(os.path.join(psp_data, "pit_u.pac"), "rb").read(),
        open(os.path.join(dc_dir, "1ST_READ.BIN"), "rb").read(), b.boot.data)
    open(os.path.join(out_dir, "PITU.PAC"), "wb").write(pitu)
    patches += hud_patches
    screen_info["hud_parts"] = [hud_parts[c] for c in sorted(hud_parts)]
    print(f"fight HUD: PITU.PAC {len(pitu)} bytes")
    import files
    for name, size in files.build(psp_data, dc_dir, out_dir, [cid for cid, _ in pairs]):
        print(f"{name}: {size} bytes")
    patches += chartables.file_patches([k.lower() for k in files.KINDS])
    import throws
    throw_patches, end = throws.patches(dc_dir, psp_data, chartables.EXT_THROWS)
    assert end <= chartables.EXT_NAMES
    patches += throw_patches
    print(f"throws: {len(throw_patches) - 1} lists for the PSP characters as opponents")
    import arcade
    routes, end = arcade.patches(b.boot.data, [cid for cid, _ in pairs], chartables.EXT_ROUTES,
                                 {t.dc: t for t in chartables.layout()}[arcade.ROUTES].ext)
    assert end <= chartables.EXT_SELECT
    patches += routes
    problems, nfiles = emit_c(b, out_dir, patches, screen_info)
    for p in sorted(set(b.problems + problems))[:60]:
        print("  problem:", p)
    print(f"{len(set(b.problems + problems))} problems; C in {nfiles} files")


if __name__ == "__main__":
    main()
