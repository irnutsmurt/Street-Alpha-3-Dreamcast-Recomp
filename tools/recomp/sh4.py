"""SH-4 instruction decoder.

decode(op) -> Insn with mnemonic, operand fields and control-flow class.
Field names: n, m (register numbers), d (displacement, raw), i (immediate, raw).
"""
from dataclasses import dataclass

# Control-flow classes
FLOW_NONE = 0
FLOW_BRANCH = 1        # conditional, no delay slot (bt/bf)
FLOW_BRANCH_DS = 2     # conditional with delay slot (bt/s, bf/s)
FLOW_JUMP = 3          # unconditional pc-relative with delay slot (bra)
FLOW_CALL = 4          # bsr
FLOW_JUMP_REG = 5      # jmp @Rn, braf (delay slot)
FLOW_CALL_REG = 6      # jsr @Rn, bsrf (delay slot)
FLOW_RET = 7           # rts (delay slot)
FLOW_RTE = 8           # rte (delay slot)
FLOW_TRAP = 9          # trapa
FLOW_ILLEGAL = 10


@dataclass
class Insn:
    op: int
    name: str
    n: int = 0
    m: int = 0
    d: int = 0
    i: int = 0
    flow: int = FLOW_NONE

    @property
    def has_delay_slot(self):
        return self.flow in (FLOW_BRANCH_DS, FLOW_JUMP, FLOW_CALL, FLOW_JUMP_REG,
                             FLOW_CALL_REG, FLOW_RET, FLOW_RTE)


def _s8(x):
    return x - 0x100 if x & 0x80 else x


def _s12(x):
    return x - 0x1000 if x & 0x800 else x


def branch_target(insn, pc):
    """Target of a pc-relative branch at address pc."""
    if insn.name in ("bt", "bf", "bt/s", "bf/s"):
        return pc + 4 + _s8(insn.d) * 2
    if insn.name in ("bra", "bsr"):
        return pc + 4 + _s12(insn.d) * 2
    raise ValueError(insn.name)


def pcrel_addr(insn, pc):
    """Literal-pool address for mov.w/mov.l @(disp,PC) and mova."""
    if insn.name == "mov.w@pc":
        return pc + 4 + insn.d * 2
    if insn.name in ("mov.l@pc", "mova"):
        return ((pc + 4) & ~3) + insn.d * 4
    raise ValueError(insn.name)


# Tables keyed on (op & mask) == value, checked in order.
_TABLE = []


def _add(pattern, name, flow=FLOW_NONE):
    """pattern: 16-char string of 0/1/n/m/d/i."""
    pattern = pattern.replace(" ", "")
    assert len(pattern) == 16, pattern
    mask = value = 0
    for ch in pattern:
        mask <<= 1
        value <<= 1
        if ch in "01":
            mask |= 1
            value |= int(ch)
    fields = {}
    for idx, ch in enumerate(pattern):
        if ch in "nmdi":
            bit = 15 - idx
            lo, hi = fields.get(ch, (bit, bit))
            fields[ch] = (min(lo, bit), max(hi, bit))
    _TABLE.append((mask, value, name, flow, fields))


# --- 0000 group ---
_add("0000 nnnn 0000 0010", "stc sr")
_add("0000 nnnn 0001 0010", "stc gbr")
_add("0000 nnnn 0010 0010", "stc vbr")
_add("0000 nnnn 0011 0010", "stc ssr")
_add("0000 nnnn 0100 0010", "stc spc")
_add("0000 nnnn 1mmm 0010", "stc rbank")
_add("0000 nnnn 0000 0011", "bsrf", FLOW_CALL_REG)
_add("0000 nnnn 0010 0011", "braf", FLOW_JUMP_REG)
_add("0000 nnnn 1000 0011", "pref")
_add("0000 nnnn 1001 0011", "ocbi")
_add("0000 nnnn 1010 0011", "ocbp")
_add("0000 nnnn 1011 0011", "ocbwb")
_add("0000 nnnn 1100 0011", "movca.l")
_add("0000 nnnn mmmm 0100", "mov.b r0st")      # mov.b Rm,@(R0,Rn)
_add("0000 nnnn mmmm 0101", "mov.w r0st")
_add("0000 nnnn mmmm 0110", "mov.l r0st")
_add("0000 nnnn mmmm 0111", "mul.l")
_add("0000 0000 0000 1000", "clrt")
_add("0000 0000 0001 1000", "sett")
_add("0000 0000 0010 1000", "clrmac")
_add("0000 0000 0011 1000", "ldtlb")
_add("0000 0000 0100 1000", "clrs")
_add("0000 0000 0101 1000", "sets")
_add("0000 0000 0000 1001", "nop")
_add("0000 0000 0001 1001", "div0u")
_add("0000 nnnn 0010 1001", "movt")
_add("0000 nnnn 0000 1010", "sts mach")
_add("0000 nnnn 0001 1010", "sts macl")
_add("0000 nnnn 0010 1010", "sts pr")
_add("0000 nnnn 0011 1010", "stc sgr")
_add("0000 nnnn 0101 1010", "sts fpul")
_add("0000 nnnn 0110 1010", "sts fpscr")
_add("0000 nnnn 1111 1010", "stc dbr")
_add("0000 0000 0000 1011", "rts", FLOW_RET)
_add("0000 0000 0001 1011", "sleep")
_add("0000 0000 0010 1011", "rte", FLOW_RTE)
_add("0000 nnnn mmmm 1100", "mov.b r0ld")      # mov.b @(R0,Rm),Rn
_add("0000 nnnn mmmm 1101", "mov.w r0ld")
_add("0000 nnnn mmmm 1110", "mov.l r0ld")
_add("0000 nnnn mmmm 1111", "mac.l")
# --- 0001 ---
_add("0001 nnnn mmmm dddd", "mov.l stdisp")    # mov.l Rm,@(disp,Rn)
# --- 0010 ---
_add("0010 nnnn mmmm 0000", "mov.b st")
_add("0010 nnnn mmmm 0001", "mov.w st")
_add("0010 nnnn mmmm 0010", "mov.l st")
_add("0010 nnnn mmmm 0100", "mov.b stdec")
_add("0010 nnnn mmmm 0101", "mov.w stdec")
_add("0010 nnnn mmmm 0110", "mov.l stdec")
_add("0010 nnnn mmmm 0111", "div0s")
_add("0010 nnnn mmmm 1000", "tst")
_add("0010 nnnn mmmm 1001", "and")
_add("0010 nnnn mmmm 1010", "xor")
_add("0010 nnnn mmmm 1011", "or")
_add("0010 nnnn mmmm 1100", "cmp/str")
_add("0010 nnnn mmmm 1101", "xtrct")
_add("0010 nnnn mmmm 1110", "mulu.w")
_add("0010 nnnn mmmm 1111", "muls.w")
# --- 0011 ---
for low, nm in [(0, "cmp/eq"), (2, "cmp/hs"), (3, "cmp/ge"), (4, "div1"), (5, "dmulu.l"),
                (6, "cmp/hi"), (7, "cmp/gt"), (8, "sub"), (10, "subc"), (11, "subv"),
                (12, "add"), (13, "dmuls.l"), (14, "addc"), (15, "addv")]:
    _add("0011 nnnn mmmm " + format(low, "04b"), nm)
# --- 0100 ---
for low8, nm, flow in [
    (0x00, "shll", 0), (0x01, "shlr", 0), (0x02, "sts.l mach", 0), (0x03, "stc.l sr", 0),
    (0x04, "rotl", 0), (0x05, "rotr", 0), (0x06, "lds.l mach", 0), (0x07, "ldc.l sr", 0),
    (0x08, "shll2", 0), (0x09, "shlr2", 0), (0x0A, "lds mach", 0), (0x0B, "jsr", FLOW_CALL_REG),
    (0x0E, "ldc sr", 0),
    (0x10, "dt", 0), (0x11, "cmp/pz", 0), (0x12, "sts.l macl", 0), (0x13, "stc.l gbr", 0),
    (0x15, "cmp/pl", 0), (0x16, "lds.l macl", 0), (0x17, "ldc.l gbr", 0), (0x18, "shll8", 0),
    (0x19, "shlr8", 0), (0x1A, "lds macl", 0), (0x1B, "tas.b", 0), (0x1E, "ldc gbr", 0),
    (0x20, "shal", 0), (0x21, "shar", 0), (0x22, "sts.l pr", 0), (0x23, "stc.l vbr", 0),
    (0x24, "rotcl", 0), (0x25, "rotcr", 0), (0x26, "lds.l pr", 0), (0x27, "ldc.l vbr", 0),
    (0x28, "shll16", 0), (0x29, "shlr16", 0), (0x2A, "lds pr", 0), (0x2B, "jmp", FLOW_JUMP_REG),
    (0x2E, "ldc vbr", 0),
    (0x32, "stc.l sgr", 0), (0x33, "stc.l ssr", 0), (0x37, "ldc.l ssr", 0), (0x3E, "ldc ssr", 0),
    (0x43, "stc.l spc", 0), (0x47, "ldc.l spc", 0), (0x4E, "ldc spc", 0),
    (0x52, "sts.l fpul", 0), (0x56, "lds.l fpul", 0), (0x5A, "lds fpul", 0),
    (0x62, "sts.l fpscr", 0), (0x66, "lds.l fpscr", 0), (0x6A, "lds fpscr", 0),
    (0xF2, "stc.l dbr", 0), (0xF6, "ldc.l dbr", 0), (0xFA, "ldc dbr", 0),
]:
    _add("0100 nnnn " + format(low8, "08b"), nm, flow)
_add("0100 nnnn 1mmm 0011", "stc.l rbank")
_add("0100 nnnn 1mmm 0111", "ldc.l rbank")
_add("0100 nnnn 1mmm 1110", "ldc rbank")
_add("0100 nnnn mmmm 1100", "shad")
_add("0100 nnnn mmmm 1101", "shld")
_add("0100 nnnn mmmm 1111", "mac.w")
# --- 0101 ---
_add("0101 nnnn mmmm dddd", "mov.l lddisp")    # mov.l @(disp,Rm),Rn
# --- 0110 ---
for low, nm in [(0, "mov.b ld"), (1, "mov.w ld"), (2, "mov.l ld"), (3, "mov"),
                (4, "mov.b ldinc"), (5, "mov.w ldinc"), (6, "mov.l ldinc"), (7, "not"),
                (8, "swap.b"), (9, "swap.w"), (10, "negc"), (11, "neg"),
                (12, "extu.b"), (13, "extu.w"), (14, "exts.b"), (15, "exts.w")]:
    _add("0110 nnnn mmmm " + format(low, "04b"), nm)
# --- 0111 ---
_add("0111 nnnn iiii iiii", "add #imm")
# --- 1000 ---
_add("1000 0000 nnnn dddd", "mov.b r0stdisp")  # mov.b R0,@(disp,Rn)
_add("1000 0001 nnnn dddd", "mov.w r0stdisp")
_add("1000 0100 mmmm dddd", "mov.b r0lddisp")  # mov.b @(disp,Rm),R0
_add("1000 0101 mmmm dddd", "mov.w r0lddisp")
_add("1000 1000 iiii iiii", "cmp/eq #imm")
_add("1000 1001 dddd dddd", "bt", FLOW_BRANCH)
_add("1000 1011 dddd dddd", "bf", FLOW_BRANCH)
_add("1000 1101 dddd dddd", "bt/s", FLOW_BRANCH_DS)
_add("1000 1111 dddd dddd", "bf/s", FLOW_BRANCH_DS)
# --- 1001..1110 ---
_add("1001 nnnn dddd dddd", "mov.w@pc")
_add("1010 dddd dddd dddd", "bra", FLOW_JUMP)
_add("1011 dddd dddd dddd", "bsr", FLOW_CALL)
for low4, nm, fld, flow in [
    (0, "mov.b r0stgbr", "d", 0), (1, "mov.w r0stgbr", "d", 0), (2, "mov.l r0stgbr", "d", 0),
    (3, "trapa", "i", FLOW_TRAP), (4, "mov.b ldgbr", "d", 0), (5, "mov.w ldgbr", "d", 0),
    (6, "mov.l ldgbr", "d", 0), (7, "mova", "d", 0), (8, "tst #imm", "i", 0),
    (9, "and #imm", "i", 0), (10, "xor #imm", "i", 0), (11, "or #imm", "i", 0),
    (12, "tst.b #imm", "i", 0), (13, "and.b #imm", "i", 0), (14, "xor.b #imm", "i", 0),
    (15, "or.b #imm", "i", 0),
]:
    _add("1100 " + format(low4, "04b") + " " + fld * 8, nm, flow)
_add("1101 nnnn dddd dddd", "mov.l@pc")
_add("1110 nnnn iiii iiii", "mov #imm")
# --- 1111 FPU ---
for low, nm in [(0, "fadd"), (1, "fsub"), (2, "fmul"), (3, "fdiv"), (4, "fcmp/eq"),
                (5, "fcmp/gt"), (6, "fmov.s r0ld"), (7, "fmov.s r0st"), (8, "fmov.s ld"),
                (9, "fmov.s ldinc"), (10, "fmov.s st"), (11, "fmov.s stdec"), (12, "fmov"),
                (14, "fmac")]:
    _add("1111 nnnn mmmm " + format(low, "04b"), nm)
for mid, nm in [(0, "fsts"), (1, "flds"), (2, "float"), (3, "ftrc"), (4, "fneg"), (5, "fabs"),
                (6, "fsqrt"), (7, "fsrra"), (8, "fldi0"), (9, "fldi1"), (10, "fcnvsd"),
                (11, "fcnvds")]:
    _add("1111 nnnn " + format(mid, "04b") + " 1101", nm)
_add("1111 0011 1111 1101", "fschg")
_add("1111 1011 1111 1101", "frchg")
_add("1111 nn01 1111 1101", "ftrv")
_add("1111 nnn0 1111 1101", "fsca")
_add("1111 nnmm 1110 1101", "fipr")


def _build_lut():
    lut = [None] * 65536
    for op in range(65536):
        for mask, value, name, flow, fields in _TABLE:
            if op & mask == value:
                kw = {}
                for ch, (lo, hi) in fields.items():
                    kw[ch] = (op >> lo) & ((1 << (hi - lo + 1)) - 1)
                lut[op] = Insn(op, name, flow=flow, **kw)
                break
        else:
            lut[op] = Insn(op, "illegal", flow=FLOW_ILLEGAL)
    return lut


_LUT = _build_lut()


def decode(op):
    return _LUT[op]


def disasm(insn, pc):
    """Human-readable form (for listings / comments)."""
    n, m, d, i = insn.n, insn.m, insn.d, insn.i
    nm = insn.name
    if insn.flow in (FLOW_BRANCH, FLOW_BRANCH_DS, FLOW_JUMP, FLOW_CALL):
        return f"{nm} 0x{branch_target(insn, pc):08X}"
    if nm in ("mov.w@pc", "mov.l@pc"):
        return f"{nm[:5]} @(0x{pcrel_addr(insn, pc):08X}),r{n}"
    if nm == "mova":
        return f"mova 0x{pcrel_addr(insn, pc):08X},r0"
    if "#imm" in nm:
        return nm.replace("#imm", f"#{_s8(i) if nm.startswith(('add', 'mov', 'cmp')) else i}") + f" r{n}"
    return f"{nm} n={n} m={m} d={d}"
