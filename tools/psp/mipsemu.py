"""A small MIPS interpreter, for running a PSP character's set-up code at
build time: its entry function fills in the descriptor block (the PSP builds
part of it at load, from the archive's section 0x11) and that result goes into
the Dreamcast character file.

Memory is the PSP program plus a few scratch areas; calls to code outside the
set the caller allows are skipped (they return 0).
"""
import struct

import mips

OFF = 0x74


class Memory:
    def __init__(self, boot):
        self.boot = boot
        self.areas = []  # (base, bytearray)
        self.writes = {}  # address -> size, for the areas being watched

    def add(self, base, data):
        self.areas.append((base, data if isinstance(data, bytearray) else bytearray(data)))

    def _find(self, a, n):
        for base, buf in self.areas:
            if base <= a and a + n <= base + len(buf):
                return buf, a - base
        if 0 <= a and a + n + OFF <= len(self.boot.data):
            return None, a
        raise MemoryError(f"no memory at {a:08X}")

    def read(self, a, n):
        buf, o = self._find(a, n)
        if buf is None:
            return int.from_bytes(self.boot.data[o + OFF:o + OFF + n], "little")
        return int.from_bytes(buf[o:o + n], "little")

    def write(self, a, v, n):
        buf, o = self._find(a, n)
        if buf is None:
            raise MemoryError(f"write to the PSP program at {a:08X}")
        buf[o:o + n] = (v & ((1 << (8 * n)) - 1)).to_bytes(n, "little")
        self.writes[a] = n


class Cpu:
    def __init__(self, mem, allow):
        self.mem, self.allow = mem, allow  # allow(target) -> True: run it; False: skip
        self.r = [0] * 32
        self.hi = self.lo = 0
        self.skipped = []

    def call(self, entry, args=(), sp=0x0F000000, limit=2_000_000):
        for k, v in enumerate(args):
            self.r[4 + k] = v & 0xFFFFFFFF
        self.r[29] = sp
        self.r[31] = 0xFFFFFFF0  # returning here ends the run
        self.run(entry, limit)

    def run(self, pc, limit):
        r, mem = self.r, self.mem
        steps = 0
        while pc != 0xFFFFFFF0:
            steps += 1
            if steps > limit:
                raise RuntimeError("too many steps")
            i = mips.decode(mem.read(pc, 4), pc)
            if i.has_delay_slot or i.name == "bal":
                pc = self.branch(i, pc)
                continue
            self.step(i)
            pc += 4
        return

    def s(self, x):
        return x - (1 << 32) if x & 0x80000000 else x

    def branch(self, i, pc):
        r = self.r
        n = i.name
        ds = mips.decode(self.mem.read(pc + 4, 4), pc + 4)
        if n in ("jal", "bal", "jalr"):
            tgt = i.target if n != "jalr" else r[i.rs]
            self.step(ds)
            if self.allow(tgt):
                r[31] = pc + 8
                return tgt
            self.skipped.append(tgt)
            r[2] = 0
            return pc + 8
        if n == "jr":
            tgt = r[i.rs]
            self.step(ds)
            return tgt
        if n in ("j", "b"):
            self.step(ds)
            return i.target
        cond = {"beq": lambda: r[i.rs] == r[i.rt], "bne": lambda: r[i.rs] != r[i.rt],
                "blez": lambda: self.s(r[i.rs]) <= 0, "bgtz": lambda: self.s(r[i.rs]) > 0,
                "bltz": lambda: self.s(r[i.rs]) < 0, "bgez": lambda: self.s(r[i.rs]) >= 0}
        base = n[:-1] if n.endswith("l") and n not in ("bal",) else n
        c = cond[base]()
        if i.flow == mips.FLOW_BRANCH_LIKELY:
            if c:
                self.step(ds)
                return i.target
            return pc + 8
        self.step(ds)
        return i.target if c else pc + 8

    def step(self, i):
        r, n = self.r, i.name
        M = 0xFFFFFFFF

        def w(reg, v):
            if reg:
                r[reg] = v & M
        if n == "nop":
            return
        if n == "lui":
            return w(i.rt, i.imm << 16)
        if n in ("addiu", "addi"):
            return w(i.rt, r[i.rs] + i.imm)
        if n == "ori":
            return w(i.rt, r[i.rs] | i.imm)
        if n == "andi":
            return w(i.rt, r[i.rs] & i.imm)
        if n == "xori":
            return w(i.rt, r[i.rs] ^ i.imm)
        if n == "slti":
            return w(i.rt, int(self.s(r[i.rs]) < i.imm))
        if n == "sltiu":
            return w(i.rt, int(r[i.rs] < (i.imm & M)))
        if n in ("addu", "add"):
            return w(i.rd, r[i.rs] + r[i.rt])
        if n in ("subu", "sub"):
            return w(i.rd, r[i.rs] - r[i.rt])
        if n == "and":
            return w(i.rd, r[i.rs] & r[i.rt])
        if n == "or":
            return w(i.rd, r[i.rs] | r[i.rt])
        if n == "xor":
            return w(i.rd, r[i.rs] ^ r[i.rt])
        if n == "nor":
            return w(i.rd, ~(r[i.rs] | r[i.rt]))
        if n == "slt":
            return w(i.rd, int(self.s(r[i.rs]) < self.s(r[i.rt])))
        if n == "sltu":
            return w(i.rd, int(r[i.rs] < r[i.rt]))
        if n == "sll":
            return w(i.rd, r[i.rt] << i.sa)
        if n == "srl":
            return w(i.rd, r[i.rt] >> i.sa)
        if n == "sra":
            return w(i.rd, self.s(r[i.rt]) >> i.sa)
        if n == "sllv":
            return w(i.rd, r[i.rt] << (r[i.rs] & 31))
        if n == "srlv":
            return w(i.rd, r[i.rt] >> (r[i.rs] & 31))
        if n == "srav":
            return w(i.rd, self.s(r[i.rt]) >> (r[i.rs] & 31))
        if n == "movz":
            if not r[i.rt]:
                w(i.rd, r[i.rs])
            return
        if n == "movn":
            if r[i.rt]:
                w(i.rd, r[i.rs])
            return
        if n == "seb":
            return w(i.rd, (r[i.rt] & 0xFF) - (0x100 if r[i.rt] & 0x80 else 0))
        if n == "seh":
            return w(i.rd, (r[i.rt] & 0xFFFF) - (0x10000 if r[i.rt] & 0x8000 else 0))
        if n in ("mult", "multu"):
            a, b = (self.s(r[i.rs]), self.s(r[i.rt])) if n == "mult" else (r[i.rs], r[i.rt])
            p = (a * b) & 0xFFFFFFFFFFFFFFFF
            self.lo, self.hi = p & M, p >> 32
            return
        if n in ("div", "divu"):
            a, b = (self.s(r[i.rs]), self.s(r[i.rt])) if n == "div" else (r[i.rs], r[i.rt])
            if b:
                q = int(a / b)
                self.lo, self.hi = q & M, (a - q * b) & M
            return
        if n == "mfhi":
            return w(i.rd, self.hi)
        if n == "mflo":
            return w(i.rd, self.lo)
        if n in ("max", "min"):
            a, b = self.s(r[i.rs]), self.s(r[i.rt])
            return w(i.rd, max(a, b) if n == "max" else min(a, b))
        sizes = {"lb": 1, "lbu": 1, "lh": 2, "lhu": 2, "lw": 4, "sb": 1, "sh": 2, "sw": 4}
        if n in sizes:
            a = (r[i.rs] + i.imm) & M
            k = sizes[n]
            if n[0] == "l":
                v = self.mem.read(a, k)
                if n in ("lb", "lh") and v & (1 << (8 * k - 1)):
                    v -= 1 << (8 * k)
                return w(i.rt, v)
            self.mem.write(a, r[i.rt], k)
            return
        if n == "cache" or n.startswith("c.") or n in ("mtc1", "mfc1", "lwc1", "swc1"):
            return
        raise NotImplementedError(n)
