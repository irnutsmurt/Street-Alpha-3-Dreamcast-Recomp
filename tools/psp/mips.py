"""MIPS (PSP Allegrex) instruction decoder: the subset the game's character code uses.

decode(word, pc) -> Insn: name, rs, rt, rd, sa, imm (sign-extended where the
instruction treats it so), target (branches / jumps), and flow class.
"""
from dataclasses import dataclass

FLOW_NONE, FLOW_BRANCH, FLOW_BRANCH_LIKELY, FLOW_JUMP, FLOW_CALL, FLOW_JUMP_REG, FLOW_CALL_REG = range(7)

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


@dataclass
class Insn:
    word: int
    name: str
    rs: int = 0
    rt: int = 0
    rd: int = 0
    sa: int = 0
    imm: int = 0
    target: int = None
    flow: int = FLOW_NONE
    fs: int = 0
    ft: int = 0
    fd: int = 0

    @property
    def has_delay_slot(self):
        return self.flow != FLOW_NONE


def _s16(x):
    return x - 0x10000 if x & 0x8000 else x


SPECIAL = {0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
           0x08: "jr", 0x09: "jalr", 0x0A: "movz", 0x0B: "movn", 0x10: "mfhi", 0x11: "mthi",
           0x12: "mflo", 0x13: "mtlo", 0x16: "clz", 0x17: "clo", 0x18: "mult", 0x19: "multu",
           0x1A: "div", 0x1B: "divu", 0x1C: "madd", 0x1D: "maddu", 0x20: "add", 0x21: "addu",
           0x22: "sub", 0x23: "subu", 0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor",
           0x2A: "slt", 0x2B: "sltu", 0x2C: "max", 0x2D: "min", 0x2E: "msub", 0x2F: "msubu"}
IMM = {0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori",
       0x0E: "xori", 0x0F: "lui"}
MEM = {0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr",
       0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr", 0x31: "lwc1", 0x39: "swc1"}
BRANCH = {0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
          0x14: "beql", 0x15: "bnel", 0x16: "blezl", 0x17: "bgtzl"}
REGIMM = {0x00: "bltz", 0x01: "bgez", 0x02: "bltzl", 0x03: "bgezl", 0x10: "bltzal", 0x11: "bgezal"}
COP1_S = {0x00: "add.s", 0x01: "sub.s", 0x02: "mul.s", 0x03: "div.s", 0x04: "sqrt.s", 0x05: "abs.s",
          0x06: "mov.s", 0x07: "neg.s", 0x0C: "round.w.s", 0x0D: "trunc.w.s", 0x0E: "ceil.w.s",
          0x0F: "floor.w.s", 0x24: "cvt.w.s"}


def decode(w, pc):
    op = w >> 26
    rs, rt, rd, sa, fn = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xFFFF
    if op == 0:
        name = SPECIAL.get(fn)
        if name is None:
            return Insn(w, "?")
        if name == "sll" and w == 0:
            return Insn(w, "nop")
        flow = FLOW_JUMP_REG if name == "jr" else FLOW_CALL_REG if name == "jalr" else FLOW_NONE
        return Insn(w, name, rs, rt, rd, sa, flow=flow)
    if op == 0x1F:  # Allegrex SPECIAL3: ext / ins / seb / seh / wsbh
        if fn == 0x00:
            return Insn(w, "ext", rs, rt, rd, sa)
        if fn == 0x04:
            return Insn(w, "ins", rs, rt, rd, sa)
        if fn == 0x20:
            sub = {0x10: "seb", 0x18: "seh", 0x02: "wsbh", 0x14: "bitrev"}.get(sa, "?")
            return Insn(w, sub, rs, rt, rd)
        return Insn(w, "?")
    if op == 0x1C:  # SPECIAL2 (Allegrex: halt, mfic...): unused here
        return Insn(w, "?")
    if op in (0x02, 0x03):
        t = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
        return Insn(w, "j" if op == 2 else "jal", target=t, flow=FLOW_JUMP if op == 2 else FLOW_CALL)
    if op in BRANCH:
        name = BRANCH[op]
        t = pc + 4 + (_s16(imm) << 2)
        if name == "beq" and rs == 0 and rt == 0:
            return Insn(w, "b", target=t, flow=FLOW_JUMP)
        return Insn(w, name, rs, rt, imm=_s16(imm), target=t,
                    flow=FLOW_BRANCH_LIKELY if name.endswith("l") else FLOW_BRANCH)
    if op == 0x01:
        name = REGIMM.get(rt)
        if name is None:
            return Insn(w, "?")
        t = pc + 4 + (_s16(imm) << 2)
        if name == "bgezal" and rs == 0:
            return Insn(w, "bal", target=t, flow=FLOW_CALL)
        flow = FLOW_BRANCH_LIKELY if name.endswith("l") and not name.endswith("al") else FLOW_BRANCH
        return Insn(w, name, rs, rt, imm=_s16(imm), target=t, flow=flow)
    if op in IMM:
        name = IMM[op]
        v = imm if name in ("andi", "ori", "xori", "lui") else _s16(imm)
        return Insn(w, name, rs, rt, imm=v)
    if op in MEM:
        return Insn(w, MEM[op], rs, rt, imm=_s16(imm), ft=rt)
    if op == 0x11:  # COP1
        fmt = rs
        if fmt == 0x00:
            return Insn(w, "mfc1", rt=rt, fs=rd)
        if fmt == 0x04:
            return Insn(w, "mtc1", rt=rt, fs=rd)
        if fmt == 0x02:
            return Insn(w, "cfc1", rt=rt, fs=rd)
        if fmt == 0x06:
            return Insn(w, "ctc1", rt=rt, fs=rd)
        if fmt == 0x08:  # bc1f / bc1t (/l)
            name = ["bc1f", "bc1t", "bc1fl", "bc1tl"][rt & 3]
            t = pc + 4 + (_s16(imm) << 2)
            return Insn(w, name, imm=_s16(imm), target=t,
                        flow=FLOW_BRANCH_LIKELY if name.endswith("l") else FLOW_BRANCH)
        ft, fs, fd = rt, rd, sa
        if fmt == 0x10:  # single
            if fn >= 0x30:
                cond = ["c.f.s", "c.un.s", "c.eq.s", "c.ueq.s", "c.olt.s", "c.ult.s", "c.ole.s", "c.ule.s",
                        "c.sf.s", "c.ngle.s", "c.seq.s", "c.ngl.s", "c.lt.s", "c.nge.s", "c.le.s", "c.ngt.s"][fn & 15]
                return Insn(w, cond, fs=fs, ft=ft)
            name = COP1_S.get(fn)
            return Insn(w, name or "?", fs=fs, ft=ft, fd=fd)
        if fmt == 0x14 and fn == 0x20:
            return Insn(w, "cvt.s.w", fs=fs, fd=fd)
        return Insn(w, "?")
    if op == 0x2F:  # cache
        return Insn(w, "cache")
    return Insn(w, "?")
