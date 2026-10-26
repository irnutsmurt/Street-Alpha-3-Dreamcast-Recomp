"""MIPS (PSP) to C, for the PSP-only characters added to the Dreamcast game.

The generated functions run on the Dreamcast port's runtime (runtime/psp.c):
  m.r[n]       the MIPS registers; $sp is the SH-4 stack pointer (g.r[15]), so
               both kinds of code share one stack
  psp_add      pointer + offset where the pointer may be a fighter or an effect
               object: the PSP lays them out differently, so a PSP offset is
               turned into the Dreamcast's (registers always hold Dreamcast
               addresses)
  psp_xa       a PSP address made by an address-forming instruction pair (the
               PRX relocations mark them) turned into the Dreamcast address
  psp_l*/s*    loads and stores (Dreamcast memory, or the side storage for
               fields only the PSP has)
  psp_call_dc  a call into the Dreamcast engine (arguments a0-a3 -> r4-r7)
  psp_icall    a call through a pointer (one of ours, or the engine's)
"""
import struct

import mips

OFF = 0x74          # BOOT.BIN: file offset = address + 0x74
REL_TEXT = (0x2D5C18, 0x7B9F0)
REL_DATA = (0x353CD0, 0x31E00)
R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6
TEXT_END = 0x1F8354
RODATA, DATA = 0x1F8FF0, 0x1FEDE0   # .rodata (no relocations in it) and .data
# Segment 1 (.bss) is relocated relative to 0x2D4F30: its addresses are offsets into it (0-0xA4D4E4),
# which the tools use as they are. Those below the .bss's start (0x2D4FC0) would read as segment 0's
# (code, data): moved out of their way to SEG1_LOW + offset.
BSS_START, SEG1_LOW = 0x2D4FC0, 0x1000000


# instructions given other C than their own: {PSP address: C} (the add-on's changes: build_char.py)
REPLACE = {}


class Boot:
    """The PSP program (BOOT.BIN) and its relocations."""
    def __init__(self, path):
        self.data = open(path, "rb").read()
        self.text_rel = {}
        self.text_seg1 = set()   # relocated instructions whose address is in segment 1
        for k in range(0, REL_TEXT[1], 8):
            o, info = struct.unpack_from("<II", self.data, REL_TEXT[0] + k)
            self.text_rel[o] = info & 0xFF
            if (info >> 16) & 0xFF == 1:
                self.text_seg1.add(o)
        self.data_ptrs = set()
        self.data_seg1 = set()
        for k in range(0, REL_DATA[1], 8):
            o, info = struct.unpack_from("<II", self.data, REL_DATA[0] + k)
            if info & 0xFF == R_MIPS_32:
                self.data_ptrs.add(o)
                if (info >> 16) & 0xFF == 1:
                    self.data_seg1.add(o)

    def canon(self, v, a):
        """the address the instruction at a builds (value v): a low segment 1 one moved to SEG1_LOW"""
        return v + SEG1_LOW if a in self.text_seg1 and v < BSS_START else v

    def ptr(self, p):
        """the pointer at p in the data (a low segment 1 one moved to SEG1_LOW)"""
        v = self.u32(p)
        return v + SEG1_LOW if p in self.data_seg1 and v < BSS_START else v

    def u32(self, a):
        return struct.unpack_from("<I", self.data, a + OFF)[0]

    def insn(self, a):
        return mips.decode(self.u32(a), a)


def reg(n):
    if n == 0:
        return "0u"
    if n == 29:
        return "g.r[15]"
    return f"m.r[{n}]"


def s32(x):
    return f"(int32_t){reg(x)}"


class Function:
    def __init__(self, boot, entry):
        self.boot, self.entry = boot, entry
        self.addrs, self.labels, self.tables = self.explore()

    def jump_table(self, a):
        """`jr rX` at a: rX loaded from a table by an lw with a LO16 relocation
        (lui hi / addu index / lw lo). Returns (lw address, table, entries) or None."""
        b = self.boot
        rx = b.insn(a).rs
        for k in range(1, 12):
            p = a - 4 * k
            i = b.insn(p)
            if i.name == "lw" and i.rt == rx and b.text_rel.get(p) == R_MIPS_LO16:
                base = i.rs
                hi = None
                for q in range(p - 4, p - 48, -4):
                    j = b.insn(q)
                    if j.name == "lui" and j.rt == base and b.text_rel.get(q) == R_MIPS_HI16:
                        hi = j.imm << 16
                        break
                    if j.name == "addu" and j.rd == base:
                        base = j.rs if b.insn(q).rs != base else j.rt  # follow the table register
                        # the lui may have set either operand; try both below
                        for r in (j.rs, j.rt):
                            for q2 in range(q - 4, q - 32, -4):
                                j2 = b.insn(q2)
                                if j2.name == "lui" and j2.rt == r and b.text_rel.get(q2) == R_MIPS_HI16:
                                    hi = j2.imm << 16
                                    break
                            if hi is not None:
                                break
                        break
                if hi is None:
                    return None
                table = (hi + i.imm) & 0xFFFFFFFF
                entries = []
                while table + 4 * len(entries) in b.data_ptrs and len(entries) < 512:
                    v = b.u32(table + 4 * len(entries))
                    if not 0 < v < TEXT_END:
                        break
                    entries.append(v)
                if not entries and RODATA <= table < DATA:
                    # .rodata isn't relocated: its tables hold the raw addresses. The
                    # count is the switch's bounds check (sltiu t, index, n) before it
                    n = self.switch_bound(p)
                    for k in range(n or 0):
                        v = b.u32(table + 4 * k)
                        if not (0 < v < TEXT_END and v % 4 == 0):
                            entries = []
                            break
                        entries.append(v)
                return p, table, entries
            if i.has_delay_slot:
                break
        return None

    def switch_bound(self, lw):
        """the case count of a switch whose table load is at lw: the n of the
        `sltiu t, index, n` (then a branch on t) a few instructions before"""
        b = self.boot
        for q in range(lw - 4, lw - 40, -4):
            j = b.insn(q)
            if j.name == "sltiu" and 0 < j.imm < 512:
                return j.imm
            if j.name in ("jr", "j", "jal", "jalr"):
                break
        return None

    def explore(self):
        b = self.boot
        seen, labels, tables, todo = set(), set(), {}, [self.entry]
        labels.add(self.entry)
        while todo:
            a = todo.pop()
            while a not in seen and 0x1000 <= a < TEXT_END:
                seen.add(a)
                i = b.insn(a)
                if i.flow == mips.FLOW_NONE or i.flow in (mips.FLOW_CALL, mips.FLOW_CALL_REG) or i.name == "bal":
                    a += 4
                    continue
                seen.add(a + 4)
                if i.flow in (mips.FLOW_BRANCH, mips.FLOW_BRANCH_LIKELY):
                    labels.add(i.target)
                    labels.add(a + 8)
                    todo.append(i.target)
                    a += 8
                    continue
                if i.name == "b":
                    labels.add(i.target)
                    a = i.target
                    continue
                if i.name == "jr" and i.rs != 31:
                    jt = self.jump_table(a)
                    if jt and jt[2]:
                        tables[a] = jt
                        for t in jt[2]:
                            labels.add(t)
                            todo.append(t)
                break  # jr ra, j elsewhere, or an unresolved jr: the path ends
        return sorted(seen), labels, tables


class Gen:
    """Turns functions into C. resolve(target) says what a call target is:
    ('psp', name) a recompiled function, ('dc', address) an engine function,
    ('nop',) a call to drop, or None (not handled: a runtime error)."""
    def __init__(self, boot, resolve):
        self.boot, self.resolve = boot, resolve
        self.problems = []
        self.tables = []

    def call(self, target, tail=False):
        r = self.resolve(target)
        if r is None:
            self.problems.append(f"call to unmapped {target:06X}")
            s = f"psp_unmapped(0x{target:06X}u);"
        elif r[0] == "psp":
            s = f"{r[1]}();"
        elif r[0] == "dc":
            s = f"psp_call_dc(0x{r[1]:08X}u);"
        else:
            s = ";"
        return s + (" return;" if tail else "")

    def xa(self, a):
        """the address translation for the address-forming instruction at a (psp_xa1: segment 1's)"""
        return "psp_xa1" if a in self.boot.text_seg1 else "psp_xa"

    def addr(self, a, i):
        """address expression for a load / store at a"""
        rel = self.boot.text_rel.get(a)
        if rel == R_MIPS_LO16:
            return f"{self.xa(a)}({reg(i.rs)} + {i.imm}u)"
        if i.rs == 29:
            return f"g.r[15] + {i.imm}u"
        if i.rs == 0:
            return f"{i.imm & 0xFFFFFFFF}u"
        return f"psp_add({reg(i.rs)}, {i.imm})"

    def op(self, a, i):
        """C for a non-control-flow instruction"""
        if a in REPLACE:
            return REPLACE[a]
        n, rs, rt, rd = i.name, i.rs, i.rt, i.rd
        R, S = reg, s32
        rel = self.boot.text_rel.get(a)

        def w(dst, expr):
            return f"{R(dst)} = {expr};" if dst else ";"
        if n == "nop" or n == "cache" or n == "sync":
            return ";"
        if n == "lui":
            return w(rt, f"0x{(i.imm << 16) & 0xFFFFFFFF:08X}u")
        if n in ("addiu", "addi"):
            if rel == R_MIPS_LO16:
                return w(rt, f"{self.xa(a)}({R(rs)} + {i.imm}u)")
            if rs == 0:
                return w(rt, f"{i.imm & 0xFFFFFFFF}u")
            if rs == 29 or rt == 29:
                return w(rt, f"{R(rs)} + {i.imm}u")
            return w(rt, f"psp_add({R(rs)}, {i.imm})")
        if n == "ori":
            if rel == R_MIPS_LO16:
                return w(rt, f"{self.xa(a)}({R(rs)} | {i.imm}u)")
            return w(rt, f"{R(rs)} | {i.imm}u")
        if n == "andi":
            return w(rt, f"{R(rs)} & {i.imm}u")
        if n == "xori":
            return w(rt, f"{R(rs)} ^ {i.imm}u")
        if n == "slti":
            return w(rt, f"{S(rs)} < {i.imm}")
        if n == "sltiu":
            return w(rt, f"{R(rs)} < {i.imm & 0xFFFFFFFF}u")
        if n == "addu" or n == "add":
            if rs == 0 or rt == 0:
                return w(rd, R(rt) if rs == 0 else R(rs))
            if 29 in (rs, rt, rd):
                return w(rd, f"{R(rs)} + {R(rt)}")
            return w(rd, f"psp_addu({R(rs)}, {R(rt)})")
        if n in ("subu", "sub"):
            return w(rd, f"{R(rs)} - {R(rt)}")
        if n == "and":
            return w(rd, f"{R(rs)} & {R(rt)}")
        if n == "or":
            return w(rd, f"{R(rs)} | {R(rt)}")
        if n == "xor":
            return w(rd, f"{R(rs)} ^ {R(rt)}")
        if n == "nor":
            return w(rd, f"~({R(rs)} | {R(rt)})")
        if n == "slt":
            return w(rd, f"{S(rs)} < {S(rt)}")
        if n == "sltu":
            return w(rd, f"{R(rs)} < {R(rt)}")
        if n == "sll":
            return w(rd, f"{R(rt)} << {i.sa}")
        if n == "srl":
            return w(rd, f"{R(rt)} >> {i.sa}")
        if n == "sra":
            return w(rd, f"(uint32_t)({S(rt)} >> {i.sa})")
        if n == "sllv":
            return w(rd, f"{R(rt)} << ({R(rs)} & 31)")
        if n == "srlv":
            return w(rd, f"{R(rt)} >> ({R(rs)} & 31)")
        if n == "srav":
            return w(rd, f"(uint32_t)({S(rt)} >> ({R(rs)} & 31))")
        if n == "movz":
            return f"if (!{R(rt)}) {w(rd, R(rs))}"
        if n == "movn":
            return f"if ({R(rt)}) {w(rd, R(rs))}"
        if n == "max":
            return w(rd, f"({S(rs)} > {S(rt)} ? {R(rs)} : {R(rt)})")
        if n == "min":
            return w(rd, f"({S(rs)} < {S(rt)} ? {R(rs)} : {R(rt)})")
        if n == "clz":
            return w(rd, f"({R(rs)} ? (uint32_t)__builtin_clz({R(rs)}) : 32u)")
        if n == "clo":
            return w(rd, f"(~{R(rs)} ? (uint32_t)__builtin_clz(~{R(rs)}) : 32u)")
        if n == "seb":
            return w(rd, f"(uint32_t)(int8_t){R(rt)}")
        if n == "seh":
            return w(rd, f"(uint32_t)(int16_t){R(rt)}")
        if n == "wsbh":
            return w(rd, f"((({R(rt)} & 0x00FF00FFu) << 8) | (({R(rt)} >> 8) & 0x00FF00FFu))")
        if n == "ext":  # rt = (rs >> pos) & mask(size); pos = sa, size = rd + 1
            size = rd + 1
            mask = (1 << size) - 1 if size < 32 else 0xFFFFFFFF
            return w(rt, f"({R(rs)} >> {i.sa}) & 0x{mask:X}u")
        if n == "ins":  # rt[pos..msb] = rs; pos = sa, msb = rd
            size = rd - i.sa + 1
            mask = ((1 << size) - 1) << i.sa if size < 32 else 0xFFFFFFFF
            return w(rt, f"({R(rt)} & ~0x{mask:X}u) | (({R(rs)} << {i.sa}) & 0x{mask:X}u)")
        if n == "mult":
            return f"{{ int64_t p_ = (int64_t){S(rs)} * {S(rt)}; m.lo = (uint32_t)p_; m.hi = (uint32_t)(p_ >> 32); }}"
        if n == "multu":
            return f"{{ uint64_t p_ = (uint64_t){R(rs)} * {R(rt)}; m.lo = (uint32_t)p_; m.hi = (uint32_t)(p_ >> 32); }}"
        if n in ("madd", "msub", "maddu", "msubu"):
            sgn = "int64_t" if n in ("madd", "msub") else "uint64_t"
            cast = S if sgn == "int64_t" else R
            op = "+" if n.startswith("madd") else "-"
            return (f"{{ {sgn} acc_ = ({sgn})(((uint64_t)m.hi << 32) | m.lo); "
                    f"acc_ = acc_ {op} ({sgn}){cast(rs)} * {cast(rt)}; m.lo = (uint32_t)acc_; m.hi = (uint32_t)((uint64_t)acc_ >> 32); }}")
        if n == "div":
            return (f"if ({R(rt)}) {{ m.lo = (uint32_t)({S(rs)} / {S(rt)}); "
                    f"m.hi = (uint32_t)({S(rs)} % {S(rt)}); }}")
        if n == "divu":
            return f"if ({R(rt)}) {{ m.lo = {R(rs)} / {R(rt)}; m.hi = {R(rs)} % {R(rt)}; }}"
        if n == "mfhi":
            return w(rd, "m.hi")
        if n == "mflo":
            return w(rd, "m.lo")
        if n == "mthi":
            return f"m.hi = {R(rs)};"
        if n == "mtlo":
            return f"m.lo = {R(rs)};"
        loads = {"lb": "psp_lb", "lbu": "psp_lbu", "lh": "psp_lh", "lhu": "psp_lhu", "lw": "psp_lw"}
        stores = {"sb": "psp_sb", "sh": "psp_sh", "sw": "psp_sw"}
        if n in loads:
            return w(rt, f"{loads[n]}({self.addr(a, i)})")
        if n in stores:
            return f"{stores[n]}({self.addr(a, i)}, {R(rt)});"
        if n in ("lwl", "lwr"):
            return w(rt, f"psp_{n}({self.addr(a, i)}, {R(rt)})")
        if n in ("swl", "swr"):
            return f"psp_{n}({self.addr(a, i)}, {R(rt)});"
        if n == "lwc1":
            return f"m.f[{i.ft}].u = psp_lw({self.addr(a, i)});"
        if n == "swc1":
            return f"psp_sw({self.addr(a, i)}, m.f[{i.ft}].u);"
        if n == "mtc1":
            return f"m.f[{i.fs}].u = {R(rt)};"
        if n == "mfc1":
            return w(rt, f"m.f[{i.fs}].u")
        if n in ("cfc1", "ctc1"):
            return w(rt, "0u") if n == "cfc1" else ";"
        fop = {"add.s": "+", "sub.s": "-", "mul.s": "*", "div.s": "/"}
        if n in fop:
            return f"m.f[{i.fd}].f = m.f[{i.fs}].f {fop[n]} m.f[{i.ft}].f;"
        if n == "mov.s":
            return f"m.f[{i.fd}] = m.f[{i.fs}];"
        if n == "neg.s":
            return f"m.f[{i.fd}].f = -m.f[{i.fs}].f;"
        if n == "abs.s":
            return f"m.f[{i.fd}].f = fabsf(m.f[{i.fs}].f);"
        if n == "sqrt.s":
            return f"m.f[{i.fd}].f = sqrtf(m.f[{i.fs}].f);"
        if n == "cvt.s.w":
            return f"m.f[{i.fd}].f = (float)(int32_t)m.f[{i.fs}].u;"
        if n in ("trunc.w.s", "cvt.w.s"):
            return f"m.f[{i.fd}].u = (uint32_t)(int32_t)m.f[{i.fs}].f;"
        if n == "round.w.s":
            return f"m.f[{i.fd}].u = (uint32_t)(int32_t)lrintf(m.f[{i.fs}].f);"
        if n == "floor.w.s":
            return f"m.f[{i.fd}].u = (uint32_t)(int32_t)floorf(m.f[{i.fs}].f);"
        if n == "ceil.w.s":
            return f"m.f[{i.fd}].u = (uint32_t)(int32_t)ceilf(m.f[{i.fs}].f);"
        if n.startswith("c.") and n.endswith(".s"):
            c = n[2:-2]
            x, y = f"m.f[{i.fs}].f", f"m.f[{i.ft}].f"
            expr = {"f": "0", "un": f"({x} != {x} || {y} != {y})", "eq": f"{x} == {y}", "ueq": f"!({x} < {y} || {x} > {y})",
                    "olt": f"{x} < {y}", "ult": f"!({x} >= {y})", "ole": f"{x} <= {y}", "ule": f"!({x} > {y})",
                    "sf": "0", "ngle": f"({x} != {x} || {y} != {y})", "seq": f"{x} == {y}", "ngl": f"!({x} < {y} || {x} > {y})",
                    "lt": f"{x} < {y}", "nge": f"!({x} >= {y})", "le": f"{x} <= {y}", "ngt": f"!({x} > {y})"}[c]
            return f"m.fcc = {expr};"
        self.problems.append(f"{a:06X}: unhandled {n}")
        return f"psp_bad(0x{a:06X}u); /* {n} */"

    def cond(self, i):
        R, S = reg, s32
        n = i.name.rstrip("l") if i.name not in ("bltzl", "bgezl", "blezl", "bgtzl", "beql", "bnel") else i.name[:-1]
        if n == "beq":
            return f"{R(i.rs)} == {R(i.rt)}"
        if n == "bne":
            return f"{R(i.rs)} != {R(i.rt)}"
        if n == "blez":
            return f"{S(i.rs)} <= 0"
        if n == "bgtz":
            return f"{S(i.rs)} > 0"
        if n == "bltz":
            return f"{S(i.rs)} < 0"
        if n == "bgez":
            return f"{S(i.rs)} >= 0"
        if n in ("bc1t", "bc1tl"):
            return "m.fcc"
        if n in ("bc1f", "bc1fl"):
            return "!m.fcc"
        raise ValueError(i.name)

    def function(self, fn, name):
        b = self.boot
        addrs = set(fn.addrs)
        out = [f"void {name}(void) {{"]
        skip = set()
        order = fn.addrs
        for idx, a in enumerate(order):
            if a in skip:
                continue
            if a in fn.labels:
                out.append(f"L_{a:06X}:;")
            i = b.insn(a)
            if not i.has_delay_slot and i.name != "bal":
                out.append("  " + self.op(a, i))
                nxt = order[idx + 1] if idx + 1 < len(order) else None
                if nxt != a + 4 and a + 4 not in skip:
                    out.append(f"  goto L_{a + 4:06X};" if a + 4 in addrs else f"  /* falls out at {a + 4:06X} */ return;")
                    fn.labels.add(a + 4) if a + 4 in addrs else None
                continue
            ds = b.insn(a + 4)
            ds_c = self.op(a + 4, ds) if ds.has_delay_slot is False else f"psp_bad(0x{a + 4:06X}u);"
            skip.add(a + 4)
            back = "IRQ_CHECK(); " if i.target is not None and i.target <= a else ""
            n = i.name
            if i.flow == mips.FLOW_BRANCH:
                out.append(f"  {{ int c_ = {self.cond(i)}; {ds_c} if (c_) {{ {back}goto L_{i.target:06X}; }} }}")
            elif i.flow == mips.FLOW_BRANCH_LIKELY:
                out.append(f"  if ({self.cond(i)}) {{ {ds_c} {back}goto L_{i.target:06X}; }}")
            elif n == "b":
                out.append(f"  {ds_c} {back}goto L_{i.target:06X};")
            elif n == "j":
                if i.target in addrs:
                    out.append(f"  {ds_c} {back}goto L_{i.target:06X};")
                else:
                    out.append(f"  {ds_c} {self.call(i.target, tail=True)}")
            elif n in ("jal", "bal"):
                out.append(f"  {ds_c} {self.call(i.target)}")
            elif n == "jalr":
                out.append(f"  {{ uint32_t t_ = {reg(i.rs)}; {ds_c} psp_icall(t_); }}")
            elif n == "jr" and i.rs == 31:
                out.append(f"  {ds_c} return;")
            elif n == "jr" and a in fn.tables:
                lw_at, table, entries = fn.tables[a]
                cases = " ".join(f"case 0x{t:06X}u: goto L_{t:06X};" for t in sorted(set(entries)))
                out.append(f"  {{ uint32_t t_ = {reg(i.rs)}; {ds_c} switch (t_) {{ {cases} default: psp_bad(0x{a:06X}u); return; }} }}")
            elif n == "jr":
                out.append(f"  {{ uint32_t t_ = {reg(i.rs)}; {ds_c} psp_icall(t_); return; }}")
            else:
                self.problems.append(f"{a:06X}: unhandled branch {n}")
                out.append(f"  psp_bad(0x{a:06X}u);")
            # not taken / after the call: continue at a + 8
            nxt = None
            for later in order[idx + 1:]:
                if later not in skip:
                    nxt = later
                    break
            if a + 4 in fn.labels:  # something jumps into the delay slot itself
                out.append(f"  goto L_{a + 8:06X};")
                out.append(f"L_{a + 4:06X}:;")
                out.append("  " + ds_c)
                fn.labels.add(a + 8)
            if n not in ("b", "j", "jr") and nxt != a + 8:
                if a + 8 in addrs:
                    out.append(f"  goto L_{a + 8:06X};")
                    fn.labels.add(a + 8)
                else:
                    out.append("  return;")
        out.append("}")
        # jump-table loads read the PSP's own table (raw PSP code addresses for the switch)
        text = "\n".join(out)
        for jr_at, (lw_at, table, entries) in fn.tables.items():
            i = b.insn(lw_at)
            arr = f"jt_{table:06X}"
            if arr not in self.tables:
                self.tables.append(arr)
                self.table_defs = getattr(self, "table_defs", [])
                self.table_defs.append(f"static const uint32_t {arr}[{len(entries)}] = {{" +
                                       ", ".join(f"0x{e:06X}u" for e in entries) + "};")
            old = f"{reg(i.rt)} = psp_lw({self.xa(lw_at)}({reg(i.rs)} + {i.imm}u));"
            new = (f"{reg(i.rt)} = {arr}[(({reg(i.rs)} + {i.imm}u) - 0x{table:06X}u) >> 2 < {len(entries)}u ? "
                   f"(({reg(i.rs)} + {i.imm}u) - 0x{table:06X}u) >> 2 : 0];")
            text = text.replace(old, new)
        return text
