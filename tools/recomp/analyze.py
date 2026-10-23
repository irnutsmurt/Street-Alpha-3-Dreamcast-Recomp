"""Function discovery for an SH-4 binary image.

Finds functions by recursive descent from the entry point, following bsr/jsr
targets (resolving `mov.l @(disp,PC),Rn ; jsr @Rn` style calls via a simple
per-block constant tracker), then expands with code pointers found in data.
"""
import struct

from sh4 import (FLOW_BRANCH, FLOW_BRANCH_DS, FLOW_CALL, FLOW_CALL_REG, FLOW_ILLEGAL,
                 FLOW_JUMP, FLOW_JUMP_REG, FLOW_RET, FLOW_RTE, branch_target, decode,
                 pcrel_addr)


class Image:
    def __init__(self, data, base):
        self.data, self.base = data, base
        self.end = base + len(data)

    def contains(self, addr):
        return self.base <= addr < self.end

    def u16(self, addr):
        return struct.unpack_from("<H", self.data, addr - self.base)[0]

    def u32(self, addr):
        return struct.unpack_from("<I", self.data, addr - self.base)[0]


def canon(addr):
    """Map P1/P2 (cached/uncached) mirrors of main RAM onto the P1 address."""
    if 0x0C000000 <= (addr & 0x1FFFFFFF) < 0x10000000:
        return 0x80000000 | (addr & 0x1FFFFFFF)
    return addr


class Function:
    def __init__(self, entry):
        self.entry = entry
        self.insns = {}          # addr -> Insn  (includes delay slots)
        self.labels = set()      # in-function branch targets
        self.calls = set()       # direct call targets
        self.tailcalls = set()   # bra/jmp to other functions
        self.indirect = []       # addrs of unresolved jmp/jsr/braf/bsrf
        self.pool = set()        # literal-pool addresses read
        self.consts = set()      # 32-bit literal values loaded (candidate code pointers)
        self.bad = None          # reason this isn't valid code
        self.jumptables = {}     # braf addr -> list of target addrs
        self.ptr_tables = set()  # bases of code-pointer tables indexed before jmp/jsr
        self.callsites = {}      # call insn addr -> resolved target


def jump_table(img, table, base):
    """Read a table of signed 16-bit offsets (relative to base) used by braf.

    The table ends where the first case's code begins (tables are emitted
    directly before the case bodies by the Hitachi/GCC compilers)."""
    targets = []
    end = table + 0x400
    a = table
    while a < end:
        v = img.u16(a)
        if v & 0x8000:
            v -= 0x10000
        t = base + v
        if t & 1 or not img.contains(t):
            break
        targets.append(t)
        if t > a:
            end = min(end, t)
        a += 2
    return targets


def code_pointer_table(img, base, limit=1024):
    """Code pointers in a jump table (byte or word indexed, so bounded).

    Unused slots are null; the table ends at the first other non-pointer."""
    out = []
    for k in range(limit):
        a = base + 4 * k
        if not img.contains(a + 3):
            break
        raw = img.u32(a)
        if raw == 0:
            continue
        v = canon(raw)
        if v & 1 or not img.contains(v):
            break
        out.append(v)
    return out


# Calls that suspend the current task; execution later resumes at the
# instruction after the call, entered from the scheduler.
YIELD_FUNCS = {0x8C0100C8}
SAVED_REGS = range(8, 15)  # callee-saved under the SH-4 ABI (r15 excluded)


def explore(img, entry, known_entries):
    """Explore one function. Branches stay inside; calls are recorded."""
    fn = Function(entry)
    work = [(entry, {})]
    seen_blocks = set()
    while work:
        pc, regs = work.pop()
        if pc in seen_blocks:
            continue
        seen_blocks.add(pc)
        fn.labels.add(pc)
        regs = dict(regs)  # crude constant tracking, inherited from the first predecessor
        tabreg = {}  # reg -> table base, for `mov.l @(r0,Rbase),Rn ; jmp @Rn`
        last_mova = None
        prev = None
        while True:
            if not img.contains(pc) or pc & 1:
                fn.bad = f"pc out of image 0x{pc:08X}"
                return fn
            if pc in fn.insns and pc != entry and pc not in fn.labels:
                break
            ins = decode(img.u16(pc))
            if ins.flow == FLOW_ILLEGAL:
                fn.bad = f"illegal op 0x{ins.op:04X} at 0x{pc:08X}"
                return fn
            fn.insns[pc] = ins
            name = ins.name
            # constant tracking
            if name == "mov.l@pc":
                a = pcrel_addr(ins, pc)
                fn.pool.update((a, a + 2))
                regs[ins.n] = img.u32(a) if img.contains(a) else None
                if regs[ins.n] is not None:
                    fn.consts.add(canon(regs[ins.n]))
            elif name == "mov.w@pc":
                a = pcrel_addr(ins, pc)
                fn.pool.add(a)
                regs.pop(ins.n, None)
            elif name == "mova":
                regs[0] = last_mova = pcrel_addr(ins, pc)
            elif name == "mov":
                if ins.m in regs:
                    regs[ins.n] = regs[ins.m]
                else:
                    regs.pop(ins.n, None)
            elif name in ("or", "add", "and", "xor") and ins.n in regs and ins.m in regs \
                    and regs[ins.n] is not None and regs[ins.m] is not None:
                a, b = regs[ins.n], regs[ins.m]
                regs[ins.n] = {"or": a | b, "add": (a + b) & 0xFFFFFFFF, "and": a & b,
                               "xor": a ^ b}[name]
            elif name == "add #imm" and regs.get(ins.n) is not None:
                regs[ins.n] = (regs[ins.n] + (ins.i - 256 if ins.i & 0x80 else ins.i)) & 0xFFFFFFFF
            elif name == "mov.l r0ld" and (regs.get(ins.m) is not None or regs.get(0) is not None):
                # mov.l @(R0,Rm),Rn: either operand may hold the table base
                cands = [canon(regs[r]) for r in (ins.m, 0) if regs.get(r) is not None]
                base = next((c for c in cands if img.contains(c)), None)
                if base is not None:
                    tabreg[ins.n] = base
                regs.pop(ins.n, None)
            elif name == "mov #imm":
                regs[ins.n] = (ins.i - 256 if ins.i & 0x80 else ins.i) & 0xFFFFFFFF
            elif ins.n in regs and name not in ("mov.l st", "mov.b st", "mov.w st", "tst",
                                                "cmp/eq", "cmp/hs", "cmp/ge", "cmp/hi",
                                                "cmp/gt", "jsr", "jmp", "braf", "bsrf"):
                regs.pop(ins.n, None)
            if ins.flow == 0 or ins.flow == 9:
                prev = ins
                pc += 2
                continue
            # delay slot
            if ins.has_delay_slot:
                ds = decode(img.u16(pc + 2))
                if ds.has_delay_slot or ds.flow in (FLOW_BRANCH, FLOW_ILLEGAL):
                    fn.bad = f"bad delay slot at 0x{pc + 2:08X}"
                    return fn
                fn.insns[pc + 2] = ds
                if ds.name == "mov.l@pc":
                    a = pcrel_addr(ds, pc + 2)
                    fn.pool.update((a, a + 2))
                    if img.contains(a):
                        fn.consts.add(canon(img.u32(a)))
            if ins.flow in (FLOW_BRANCH, FLOW_BRANCH_DS):
                work.append((branch_target(ins, pc), dict(regs)))
                pc += 4 if ins.flow == FLOW_BRANCH_DS else 2
                if ins.flow == FLOW_BRANCH:
                    continue
                work.append((pc, dict(regs)))
                break
            if ins.flow == FLOW_JUMP:
                t = branch_target(ins, pc)
                if t in known_entries and t != entry:
                    fn.tailcalls.add(t)
                else:
                    work.append((t, regs))
                break
            if ins.flow == FLOW_CALL:
                t = branch_target(ins, pc)
                fn.calls.add(t)
                fn.callsites[pc] = t
                regs = {r: v for r, v in regs.items() if r in SAVED_REGS}
                pc += 4
                continue
            if ins.flow in (FLOW_CALL_REG, FLOW_JUMP_REG) and ins.n in tabreg:
                fn.ptr_tables.add(tabreg[ins.n])
            if ins.flow == FLOW_CALL_REG:
                if ins.name == "jsr" and regs.get(ins.n) is not None:
                    fn.calls.add(canon(regs[ins.n]))
                    fn.callsites[pc] = canon(regs[ins.n])
                else:
                    fn.indirect.append(pc)
                regs = {r: v for r, v in regs.items() if r in SAVED_REGS}
                tabreg = {}
                pc += 4
                continue
            if ins.flow == FLOW_JUMP_REG:
                if (ins.name == "braf" and prev is not None and prev.name == "mov.w r0ld"
                        and last_mova is not None):
                    targets = jump_table(img, last_mova, pc + 4)
                    fn.jumptables[pc] = targets
                    fn.pool.update(range(last_mova, last_mova + 2 * len(targets), 2))
                    work.extend((t, regs) for t in targets)
                elif ins.name == "jmp" and regs.get(ins.n) is not None:
                    fn.tailcalls.add(canon(regs[ins.n]))
                else:
                    fn.indirect.append(pc)
                break
            if ins.flow in (FLOW_RET, FLOW_RTE):
                break
    return fn


def has_prologue(img, a, window=4):
    """sts.l pr,@-r15 / mov.l Rm,@-r15 / fmov.s FRm,@-r15 / add #-n,r15 within
    the first few instructions."""
    for k in range(window):
        if not img.contains(a + 2 * k + 1):
            return False
        op = img.u16(a + 2 * k)
        if (op == 0x4F22 or (op & 0xFF0F) == 0x2F06 or (op & 0xFF0F) == 0xFF0B
                or (op & 0xFF80) == 0x7F80):
            return True
    return False


def discover(img, entries, data_scan=True):
    funcs = {}
    queue = list(entries)
    known = set(entries)
    while True:
        while queue:
            e = queue.pop()
            if e in funcs or not img.contains(e):
                continue
            fn = explore(img, e, known)
            funcs[e] = fn
            if fn.bad:
                continue
            for t in fn.calls | fn.tailcalls:
                if t not in known:
                    known.add(t)
                    queue.append(t)
        # literal-pool constants that point at code are function pointers
        # (possibly mid-function entries); try them as entries
        lit = set()
        for fn in funcs.values():
            if not fn.bad:
                lit.update(c for c in fn.consts if img.contains(c) and not c & 1 and c not in known)
        # functions that tail-call the task switcher suspend their caller;
        # the scheduler later resumes at the return address of that call
        yields = set(YIELD_FUNCS)
        while True:
            more = {e for e, fn in funcs.items() if not fn.bad and e not in yields
                    and fn.tailcalls & yields}
            if not more:
                break
            yields |= more
        resume = {pc + 4 for fn in funcs.values() if not fn.bad
                  for pc, t in fn.callsites.items() if t in yields} - known
        known |= resume
        queue.extend(resume)
        if resume:
            continue
        tab = set()
        for fn in funcs.values():
            if not fn.bad:
                for base in fn.ptr_tables:
                    tab.update(code_pointer_table(img, base))
        tab -= known
        added = 0
        for v in sorted(tab):
            fn = explore(img, v, known)
            if fn.bad or len(fn.insns) < 2:
                continue
            known.add(v)
            queue.append(v)
            added += 1
        for v in sorted(lit):
            fn = explore(img, v, known)
            if fn.bad or len(fn.insns) < 2:
                continue
            if not any(i.name in ("rts", "jmp", "bra") for i in fn.insns.values()):
                continue
            known.add(v)
            queue.append(v)
            added += 1
        if added:
            continue
        if not data_scan:
            break
        # Data scan: aligned words that point at a plausible function start
        # (mov.l/sts.l pr push or any valid code) outside already-claimed code.
        claimed = set()
        for fn in funcs.values():
            if not fn.bad:
                claimed.update(fn.insns)
        new, in_table = [], set()
        for off in range(0, len(img.data) - 3, 4):
            if img.base + off in claimed:
                continue  # literal inside code; handled via consts
            v = canon(struct.unpack_from("<I", img.data, off)[0])
            if v in known or not img.contains(v) or v & 1:
                continue
            if v in claimed:
                continue
            new.append(v)
            # neighbours (skipping nulls) that are also code pointers -> table
            for step in (-4, 4):
                o = off + step
                while 0 <= o < len(img.data) - 3 and struct.unpack_from("<I", img.data, o)[0] == 0 \
                        and abs(o - off) <= 16:
                    o += step
                w = canon(struct.unpack_from("<I", img.data, o)[0]) if 0 <= o < len(img.data) - 3 else 1
                if img.contains(w) and not w & 1 and (w in known or w in claimed or w in funcs):
                    in_table.add(v)
        added = 0
        for v in new:
            fn = explore(img, v, known)
            # require a typical prologue to accept pointer-only candidates
            if fn.bad or len(fn.insns) < 3:
                continue
            # a typical prologue near the start (sts.l pr / register saves, possibly
            # after a hoisted constant load), a jump-table neighbour, or a leaf with rts
            looks_fn = (v in in_table or has_prologue(img, v)
                        or any(i.name == "rts" for i in fn.insns.values()))
            if looks_fn:
                known.add(v)
                queue.append(v)
                added += 1
        if not added:
            break
    return {k: v for k, v in funcs.items()}
