"""ARM7 (ARMv4, ARM state) static recompiler for the AICA sound driver.

The whole driver becomes one C function, arm_execute(budget), that runs
until the cycle budget is used up: direct branches are gotos, indirect
branches go through a switch on the target address, and the runtime may
deliver FIQs at branch points.
"""
import struct

COND = ["EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC", "HI", "LS", "GE", "LT", "GT", "LE", "AL", "NV"]
COND_C = {
    0: "A.z", 1: "!A.z", 2: "A.c", 3: "!A.c", 4: "A.n", 5: "!A.n", 6: "A.v", 7: "!A.v",
    8: "(A.c && !A.z)", 9: "(!A.c || A.z)", 10: "(A.n == A.v)", 11: "(A.n != A.v)",
    12: "(!A.z && A.n == A.v)", 13: "(A.z || A.n != A.v)", 14: None, 15: "0",
}
DP_OPS = ["AND", "EOR", "SUB", "RSB", "ADD", "ADC", "SBC", "RSC", "TST", "TEQ", "CMP", "CMN",
          "ORR", "MOV", "BIC", "MVN"]


def ror32(v, r):
    r &= 31
    return ((v >> r) | (v << (32 - r))) & 0xFFFFFFFF if r else v


class ArmImage:
    def __init__(self, data, base=0):
        self.data, self.base = data, base

    def contains(self, a):
        return self.base <= a < self.base + len(self.data) - 3

    def u32(self, a):
        return struct.unpack_from("<I", self.data, a - self.base)[0]


def classify(op):
    """Return (kind, info) describing control flow of an instruction."""
    cond = op >> 28
    if (op & 0x0E000000) == 0x0A000000:
        off = op & 0xFFFFFF
        if off & 0x800000:
            off -= 0x1000000
        return ("bl" if op & 0x01000000 else "b"), off * 4 + 8
    if (op & 0x0F000000) == 0x0F000000:
        return "swi", None
    # data processing writing pc
    if (op & 0x0C000000) == 0x00000000 and not is_misc(op):
        rd = (op >> 12) & 15
        opc = (op >> 21) & 15
        if rd == 15 and opc not in (8, 9, 10, 11):
            return "pcwrite", None
    if (op & 0x0C000000) == 0x04000000:  # LDR/STR
        if (op >> 20) & 1 and ((op >> 12) & 15) == 15:
            return "pcwrite", None
    if (op & 0x0E000000) == 0x08000000:  # LDM/STM
        if (op >> 20) & 1 and (op & 0x8000):
            return "pcwrite", None
    return "seq", None


def is_misc(op):
    """Multiply, swap, halfword transfers, MRS/MSR live in the DP encoding space."""
    if (op & 0x0E000090) == 0x00000090 and (op & 0x60):
        return True  # halfword / signed transfers
    if (op & 0x0FC000F0) == 0x00000090 or (op & 0x0F8000F0) == 0x00800090:
        return True  # MUL/MLA, long multiplies
    if (op & 0x0FB00FF0) == 0x01000090:
        return True  # SWP
    if (op & 0x0FBF0FFF) == 0x010F0000:
        return True  # MRS
    if (op & 0x0DB0F000) == 0x0120F000:
        return True  # MSR
    return False


def discover(img, entries):
    """Recursive descent plus jump tables, code pointers in data and prologues."""
    code = set()
    targets = set(entries)
    work = list(entries)

    def run(work):
        while work:
            pc = work.pop()
            while img.contains(pc) and pc not in code:
                op = img.u32(pc)
                if op >> 28 == 15 and (op & 0x0F000000) != 0x0F000000:
                    break  # NV-conditioned non-SWI: data
                code.add(pc)
                kind, info = classify(op)
                cond = op >> 28
                if kind in ("b", "bl"):
                    t = (pc + info) & 0xFFFFFFFF
                    targets.add(t)
                    work.append(t)
                    if kind == "bl":
                        # callees may return to lr+4 to skip the next insn
                        targets.update((pc + 4, pc + 8))
                        work.append(pc + 8)
                    if kind == "b" and cond == 14:
                        break
                elif kind == "pcwrite":
                    # add pc, pc, Rm, lsl #2 : a table of branches follows (after one slot)
                    if (op & 0x0FFFFFF0) == 0x008FF100:
                        t = pc + 8
                        while img.contains(t) and (img.u32(t) & 0x0F000000) == 0x0A000000:
                            targets.add(t)
                            work.append(t)
                            t += 4
                    if cond == 14:
                        break
                pc += 4

    run(work)
    # function prologues (stmfd sp!, {.., lr}), runs of branches (jump tables)
    # and code pointers stored in data
    size = len(img.data) & ~3
    cands = set()
    run_start = None
    for a in range(0, size, 4):
        is_b = (img.u32(a) & 0xFF000000) == 0xEA000000
        if is_b and run_start is None:
            run_start = a
        if not is_b or a + 4 >= size:
            end = a if not is_b else a + 4
            if run_start is not None and (end - run_start) // 4 >= 3:
                cands.update(range(run_start, end, 4))
            run_start = None
    for a in range(0, size - 3, 4):
        op = img.u32(a)
        if (op & 0xFFFF4000) == 0xE92D4000:
            cands.add(a)
        if op < size and not op & 3 and op >= 0x20:
            cands.add(op)
    for a in sorted(cands):
        if a not in code:
            op = img.u32(a)
            kind, _ = classify(op)
            if op >> 28 != 14 and kind == "seq":
                continue
            targets.add(a)
            run([a])
    return code, targets


# ---------------------------------------------------------------- codegen

def R(n):
    return f"A.r[{n}]"


def shifter(op, pc, need_carry):
    """C expression for operand 2 (and statement computing shifter carry into sc)."""
    if op & (1 << 25):
        imm = op & 0xFF
        rot = ((op >> 8) & 15) * 2
        v = ror32(imm, rot)
        carry = f"sc = {(v >> 31) & 1};" if (need_carry and rot) else "sc = A.c;" if need_carry else ""
        return f"0x{v:08X}u", carry, False
    rm = op & 15
    stype = (op >> 5) & 3
    if op & (1 << 4):
        rs = (op >> 8) & 15
        rmv = f"0x{pc + 12:08X}u" if rm == 15 else R(rm)
        e = f"arm_shift_reg({rmv}, {stype}, {R(rs)} & 0xFF, &sc)"
        return e, "", True
    amt = (op >> 7) & 31
    rmv = f"0x{pc + 8:08X}u" if rm == 15 else R(rm)
    if stype == 0 and amt == 0:
        return rmv, ("sc = A.c;" if need_carry else ""), False
    e = f"arm_shift_imm({rmv}, {stype}, {amt}, &sc)"
    return e, "", True


def gen_dp(op, pc):
    opc = (op >> 21) & 15
    s = (op >> 20) & 1
    rn = (op >> 16) & 15
    rd = (op >> 12) & 15
    logical = opc in (0, 1, 8, 9, 12, 13, 14, 15)
    op2, carry_stmt, _ = shifter(op, pc, s and logical)
    reg_shift = (not op & (1 << 25)) and (op & 0x10)
    rnv = f"0x{pc + (12 if reg_shift else 8):08X}u" if rn == 15 else R(rn)
    name = DP_OPS[opc]
    lines = ["uint32_t sc = A.c, c0 = A.c; (void)sc; (void)c0;", f"uint32_t b = {op2};"]
    if carry_stmt:
        lines.append(carry_stmt)
    lines.append(f"uint32_t a = {rnv}; (void)a;")
    res = {
        "AND": "a & b", "EOR": "a ^ b", "SUB": "a - b", "RSB": "b - a", "ADD": "a + b",
        "ADC": "a + b + c0", "SBC": "a - b - !c0", "RSC": "b - a - !c0", "TST": "a & b",
        "TEQ": "a ^ b", "CMP": "a - b", "CMN": "a + b", "ORR": "a | b", "MOV": "b",
        "BIC": "a & ~b", "MVN": "~b",
    }[name]
    lines.append(f"uint32_t r = {res};")
    if s:
        if rd == 15 and opc not in (8, 9, 10, 11):
            lines.append("arm_restore_spsr();")
        else:
            lines.append("A.n = r >> 31; A.z = (r == 0);")
            flags = {
                "ADD": "A.c = r < a; A.v = ((a ^ ~b) & (a ^ r)) >> 31;",
                "CMN": "A.c = r < a; A.v = ((a ^ ~b) & (a ^ r)) >> 31;",
                "ADC": "A.c = (uint32_t)(((uint64_t)a + b + c0) >> 32); A.v = ((a ^ ~b) & (a ^ r)) >> 31;",
                "SUB": "A.c = a >= b; A.v = ((a ^ b) & (a ^ r)) >> 31;",
                "CMP": "A.c = a >= b; A.v = ((a ^ b) & (a ^ r)) >> 31;",
                "RSB": "A.c = b >= a; A.v = ((b ^ a) & (b ^ r)) >> 31;",
                "SBC": "A.c = (uint64_t)a >= (uint64_t)b + !c0; A.v = ((a ^ b) & (a ^ r)) >> 31;",
                "RSC": "A.c = (uint64_t)b >= (uint64_t)a + !c0; A.v = ((b ^ a) & (b ^ r)) >> 31;",
            }
            lines.append("A.c = sc;" if logical else flags[name])
    if opc not in (8, 9, 10, 11):
        if rd == 15:
            lines.append("A.pc = r & ~3u; JUMP_DYN;")
        else:
            lines.append(f"{R(rd)} = r;")
    return " ".join(lines)


def gen_ldr_str(op, pc):
    i = (op >> 25) & 1
    p = (op >> 24) & 1
    u = (op >> 23) & 1
    b = (op >> 22) & 1
    w = (op >> 21) & 1
    load = (op >> 20) & 1
    rn = (op >> 16) & 15
    rd = (op >> 12) & 15
    if i:
        rm = op & 15
        stype = (op >> 5) & 3
        amt = (op >> 7) & 31
        off = f"arm_shift_imm({R(rm)}, {stype}, {amt}, &sc)" if (stype or amt) else R(rm)
    else:
        off = f"{op & 0xFFF}u"
    base = f"0x{pc + 8:08X}u" if rn == 15 else R(rn)
    sign = "+" if u else "-"
    lines = ["uint32_t sc = A.c; (void)sc;", f"uint32_t base = {base};", f"uint32_t off = {off};",
             f"uint32_t addr = {'base ' + sign + ' off' if p else 'base'};"]
    if load:
        lines.append(f"uint32_t v = {'arm_read8(addr)' if b else 'arm_read32_rot(addr)'};")
    else:
        val = f"0x{pc + 12:08X}u" if rd == 15 else R(rd)
        lines.append(f"{'arm_write8' if b else 'arm_write32'}(addr, {val});")
    if not p:
        lines.append(f"{R(rn)} = base {sign} off;")
    elif w:
        lines.append(f"{R(rn)} = addr;")
    if load:
        if rd == 15:
            lines.append("A.pc = v & ~3u; JUMP_DYN;")
        else:
            lines.append(f"{R(rd)} = v;")
    return " ".join(lines)


def gen_halfword(op, pc):
    p = (op >> 24) & 1
    u = (op >> 23) & 1
    imm = (op >> 22) & 1
    w = (op >> 21) & 1
    load = (op >> 20) & 1
    rn = (op >> 16) & 15
    rd = (op >> 12) & 15
    sh = (op >> 5) & 3
    off = f"{((op >> 4) & 0xF0) | (op & 15)}u" if imm else R(op & 15)
    base = f"0x{pc + 8:08X}u" if rn == 15 else R(rn)
    sign = "+" if u else "-"
    lines = [f"uint32_t base = {base};", f"uint32_t off = {off};",
             f"uint32_t addr = {'base ' + sign + ' off' if p else 'base'};"]
    if load:
        rd_expr = {1: "arm_read16(addr)", 2: "(uint32_t)(int8_t)arm_read8(addr)",
                   3: "(uint32_t)(int16_t)arm_read16(addr)"}[sh]
        lines.append(f"uint32_t v = {rd_expr};")
    else:
        lines.append(f"arm_write16(addr, {R(rd)});")
    if not p:
        lines.append(f"{R(rn)} = base {sign} off;")
    elif w:
        lines.append(f"{R(rn)} = addr;")
    if load:
        lines.append(f"{R(rd)} = v;")
    return " ".join(lines)


def gen_ldm_stm(op, pc):
    p = (op >> 24) & 1
    u = (op >> 23) & 1
    s = (op >> 22) & 1
    w = (op >> 21) & 1
    load = (op >> 20) & 1
    rn = (op >> 16) & 15
    regs = [i for i in range(16) if op & (1 << i)]
    n = len(regs)
    lines = [f"uint32_t base = {R(rn)};"]
    if u:
        start = "base + 4" if p else "base"
        final = f"base + {4 * n}"
    else:
        start = f"base - {4 * n}" if p else f"base - {4 * n} + 4"
        final = f"base - {4 * n}"
    lines.append(f"uint32_t addr = {start};")
    user_bank = s and not (load and 15 in regs)
    if load:
        if w and rn not in regs:
            lines.append(f"{R(rn)} = {final};")
        elif w:
            lines.append(f"{R(rn)} = {final};")
        for r in regs:
            if r == 15:
                lines.append("A.pc = arm_read32(addr) & ~3u;")
            elif user_bank:
                lines.append(f"arm_set_user_reg({r}, arm_read32(addr));")
            else:
                lines.append(f"{R(r)} = arm_read32(addr);")
            lines.append("addr += 4;")
        if 15 in regs:
            if s:
                lines.append("arm_restore_spsr();")
            lines.append("JUMP_DYN;")
    else:
        for r in regs:
            if r == 15:
                lines.append(f"arm_write32(addr, 0x{pc + 12:08X}u);")
            elif user_bank:
                lines.append(f"arm_write32(addr, arm_get_user_reg({r}));")
            else:
                lines.append(f"arm_write32(addr, {R(r)});")
            lines.append("addr += 4;")
        if w:
            lines.append(f"{R(rn)} = {final};")
    return " ".join(lines)


def gen_insn(op, pc, targets_code):
    """Return C for one instruction, including control transfer."""
    cond = op >> 28
    body = None
    kind, info = classify(op)
    if kind in ("b", "bl"):
        t = (pc + info) & 0xFFFFFFFF
        link = f"A.r[14] = 0x{pc + 4:08X}u; " if kind == "bl" else ""
        body = (f"{link}JUMP(0x{t:08X}u, L_{t:08X});" if t in targets_code
                else f"{link}A.pc = 0x{t:08X}u; JUMP_DYN;")
    elif kind == "swi":
        body = f"arm_swi(0x{pc + 4:08X}u); JUMP_DYN;"
    elif (op & 0x0FC000F0) == 0x00000090:  # MUL/MLA
        rd, rn, rs, rm = (op >> 16) & 15, (op >> 12) & 15, (op >> 8) & 15, op & 15
        acc = f" + {R(rn)}" if op & (1 << 21) else ""
        body = f"uint32_t r = {R(rm)} * {R(rs)}{acc}; {R(rd)} = r;"
        if op & (1 << 20):
            body += " A.n = r >> 31; A.z = r == 0;"
    elif (op & 0x0F8000F0) == 0x00800090:  # long multiply
        rdhi, rdlo, rs, rm = (op >> 16) & 15, (op >> 12) & 15, (op >> 8) & 15, op & 15
        signed = (op >> 22) & 1
        acc = (op >> 21) & 1
        t = "int64_t" if signed else "uint64_t"
        cast = "(int64_t)(int32_t)" if signed else "(uint64_t)"
        body = f"uint64_t r = (uint64_t)(({t}){cast}{R(rm)} * {cast}{R(rs)});"
        if acc:
            body += f" r += ((uint64_t){R(rdhi)} << 32) | {R(rdlo)};"
        body += f" {R(rdlo)} = (uint32_t)r; {R(rdhi)} = (uint32_t)(r >> 32);"
        if op & (1 << 20):
            body += " A.n = (uint32_t)(r >> 63); A.z = r == 0;"
    elif (op & 0x0FB00FF0) == 0x01000090:  # SWP
        rn, rd, rm = (op >> 16) & 15, (op >> 12) & 15, op & 15
        if op & (1 << 22):
            body = f"uint32_t a = {R(rn)}, t = arm_read8(a); arm_write8(a, {R(rm)}); {R(rd)} = t;"
        else:
            body = f"uint32_t a = {R(rn)}, t = arm_read32_rot(a); arm_write32(a, {R(rm)}); {R(rd)} = t;"
    elif (op & 0x0E000090) == 0x00000090 and (op & 0x60):
        body = gen_halfword(op, pc)
    elif (op & 0x0FBF0FFF) == 0x010F0000:  # MRS
        rd = (op >> 12) & 15
        body = f"{R(rd)} = {'arm_get_spsr()' if op & (1 << 22) else 'arm_get_cpsr()'};"
    elif (op & 0x0DB0F000) == 0x0120F000:  # MSR
        mask = (op >> 16) & 15
        if op & (1 << 25):
            v = f"0x{ror32(op & 0xFF, ((op >> 8) & 15) * 2):08X}u"
        else:
            v = R(op & 15)
        fn = "arm_set_spsr" if op & (1 << 22) else "arm_set_cpsr"
        body = f"{fn}({v}, {mask});"
    elif (op & 0x0C000000) == 0x00000000:
        body = gen_dp(op, pc)
    elif (op & 0x0C000000) == 0x04000000:
        body = gen_ldr_str(op, pc)
    elif (op & 0x0E000000) == 0x08000000:
        body = gen_ldm_stm(op, pc)
    else:
        body = f"arm_undefined(0x{pc:08X}u, 0x{op:08X}u);"
    c = COND_C[cond]
    if c is None:
        return f"{{ {body} }}"
    return f"if ({c}) {{ {body} }}"


def extra_entries():
    import os
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "arm_extra_entries.txt")
    out = []
    if os.path.exists(path):
        for line in open(path):
            line = line.split("#")[0].strip()
            if line:
                out.append(int(line, 16))
    return out


def generate(drv, load_offset=0x20):
    """drv: MANATEE.DRV bytes. Code is loaded at ARM address 0 from file offset 0x20."""
    data = drv[load_offset:]
    img = ArmImage(data, 0)
    entries = [0x00, 0x04, 0x08, 0x0C, 0x10, 0x18, 0x1C] + extra_entries()
    code, targets = discover(img, entries)
    addrs = sorted(code)
    out = ['#include "arm.h"', "",
           f"const uint32_t g_arm_code_size = {len(data) & ~3};",
           f"const uint32_t g_arm_code_sum = 0x{sum(struct.unpack_from('<I', data, i)[0] for i in range(0, len(data) & ~3, 4)) & 0xFFFFFFFF:08X}u;",
           "",
           "void arm_execute(int32_t budget) {",
           "  A.cycles += budget;",
           "dispatch:",
           "  if (A.cycles <= 0) return;",
           "  if (A.fiq_pending && !(A.cpsr_rest & ARM_F)) arm_enter_fiq();",
           "  switch (A.pc) {"]
    out += [f"  case 0x{a:08X}u: goto L_{a:08X};" for a in addrs]
    out += ["  default: arm_bad_pc(A.pc); return;", "  }"]
    for a in addrs:
        op = img.u32(a)
        out.append(f"L_{a:08X}: A.cycles--;")
        out.append("  " + gen_insn(op, a, code))
        if a + 4 not in code:
            # falls off the decoded region: continue dynamically
            out.append(f"  A.pc = 0x{a + 4:08X}u; goto dispatch;")
    out.append("}")
    return "\n".join(out) + "\n", len(addrs)
