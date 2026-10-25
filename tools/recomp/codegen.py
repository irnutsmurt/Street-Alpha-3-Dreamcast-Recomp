"""SH-4 -> C code generator.

Each guest function becomes `void f_XXXXXXXX(void)` operating on the global
CPU context `g` (see runtime/cpu.h). Guest branches become gotos, calls become
C calls, rts becomes return. Indirect calls go through `call_indirect()`.
"""
from sh4 import (FLOW_BRANCH, FLOW_BRANCH_DS, FLOW_CALL, FLOW_CALL_REG, FLOW_JUMP,
                 FLOW_JUMP_REG, FLOW_RET, FLOW_RTE, branch_target, pcrel_addr)


def s8(x):
    return x - 0x100 if x & 0x80 else x


def R(n):
    return f"g.r[{n}]"


def fname(addr, suffix=""):
    return f"f_{addr:08X}{suffix}"


class Unsupported(Exception):
    pass


# Address of the guest setjmp; calls to it need a host capture at the call site.
SETJMP_ADDR = 0x8C094584


# Literal-pool words the current function takes the address of with mova.
# Such a word is a variable, not a constant: e.g. the exception stub copied
# into the VBR page saves the interrupted R0 there (mova + mov.l) and reloads
# it with mov.l @(disp,PC) - folding that load to the image's value would
# return from every interrupt with R0 = 0.
_var_literals = set()

# Tables moved elsewhere (PSP characters, tools/psp): a literal inside one of
# these ranges [lo, hi) is the moved table's address plus the same offset.
RELOCATE = []


# Instructions whose constant is read from the guest memory instead: address -> where
# (tools/psp/chartables.py, IMMEDIATES)
IMMEDIATE = {}


def relocate(v):
    for lo, hi, new in RELOCATE:
        if lo <= v < hi:
            return new + (v - lo)
    return v


def emit_insn(ins, pc, img):
    """C statement(s) for a non-control-flow instruction."""
    n, m, d, i = ins.n, ins.m, ins.d, ins.i
    Rn, Rm, R0 = R(n), R(m), R(0)
    nm = ins.name
    T = "g.t"

    # ---- data movement ----
    if pc in IMMEDIATE and nm in ("mov #imm", "mov.w@pc", "mov.l@pc"):
        return f"{Rn} = R32(0x{IMMEDIATE[pc]:08X}u);"
    if pc in IMMEDIATE and nm == "add #imm":
        return f"{Rn} += R32(0x{IMMEDIATE[pc]:08X}u);"
    if pc in IMMEDIATE and nm == "cmp/eq #imm":
        return f"{T} = ({R0} == R32(0x{IMMEDIATE[pc]:08X}u));"
    if nm == "mov #imm":
        return f"{Rn} = {s8(i) & 0xFFFFFFFF}u;"
    if nm == "mov.w@pc":
        a = pcrel_addr(ins, pc)
        v = img.u16(a)
        v = (v - 0x10000 if v & 0x8000 else v) & 0xFFFFFFFF
        return f"{Rn} = 0x{v:08X}u;"
    if nm == "mov.l@pc":
        a = pcrel_addr(ins, pc)
        if a in _var_literals:  # the function writes this literal (see gen_function): read it
            return f"{Rn} = R32(0x{a:08X}u);"
        return f"{Rn} = 0x{relocate(img.u32(a)):08X}u;"
    if nm == "mova":
        return f"{R0} = 0x{pcrel_addr(ins, pc):08X}u;"
    if nm == "mov":
        return f"{Rn} = {Rm};"
    sizes = {"b": ("8", "(uint32_t)(int8_t)"), "w": ("16", "(uint32_t)(int16_t)"), "l": ("32", "")}
    if nm.startswith("mov.") and nm[4] in sizes and nm not in ("mova",):
        sz = nm[4]
        bits, sx = sizes[sz]
        scale = {"b": 1, "w": 2, "l": 4}[sz]
        kind = nm[6:]
        if kind == "st":
            return f"W{bits}({Rn}, {Rm});"
        if kind == "ld":
            return f"{Rn} = {sx}R{bits}({Rm});"
        if kind == "stdec":
            return f"{{ uint32_t a = {Rn} - {scale}; W{bits}(a, {Rm}); {Rn} = a; }}"
        if kind == "ldinc":
            inc = f" {Rm} += {scale};" if n != m else ""
            return f"{{ uint32_t v = {sx}R{bits}({Rm});{inc} {Rn} = v; }}"
        if kind == "r0st":
            return f"W{bits}({Rn} + {R0}, {Rm});"
        if kind == "r0ld":
            return f"{Rn} = {sx}R{bits}({Rm} + {R0});"
        if kind == "stdisp":       # mov.l Rm,@(disp,Rn)
            return f"W32({Rn} + {d * 4}, {Rm});"
        if kind == "lddisp":       # mov.l @(disp,Rm),Rn
            return f"{Rn} = R32({Rm} + {d * 4});"
        if kind == "r0stdisp":     # mov.x R0,@(disp,Rn)
            return f"W{bits}({Rn} + {d * scale}, {R0});"
        if kind == "r0lddisp":     # mov.x @(disp,Rm),R0
            return f"{R0} = {sx}R{bits}({Rm} + {d * scale});"
        if kind == "r0stgbr":
            return f"W{bits}(g.gbr + {d * scale}, {R0});"
        if kind == "ldgbr":
            return f"{R0} = {sx}R{bits}(g.gbr + {d * scale});"
    if nm == "movt":
        return f"{Rn} = {T};"
    if nm == "swap.b":
        return f"{Rn} = ({Rm} & 0xFFFF0000u) | (({Rm} & 0xFF) << 8) | (({Rm} >> 8) & 0xFF);"
    if nm == "swap.w":
        return f"{Rn} = ({Rm} >> 16) | ({Rm} << 16);"
    if nm == "xtrct":
        return f"{Rn} = ({Rn} >> 16) | ({Rm} << 16);"

    # ---- arithmetic ----
    if nm == "add":
        return f"{Rn} += {Rm};"
    if nm == "add #imm":
        return f"{Rn} += {s8(i) & 0xFFFFFFFF}u;"
    if nm == "addc":
        return (f"{{ uint32_t a = {Rn}, b = {Rm}, s = a + b, r = s + {T}; "
                f"{T} = (s < a) | (r < s); {Rn} = r; }}")
    if nm == "addv":
        return (f"{{ int32_t a = (int32_t){Rn}, b = (int32_t){Rm}; int32_t r; "
                f"{T} = __builtin_add_overflow(a, b, &r); {Rn} = (uint32_t)r; }}")
    if nm == "sub":
        return f"{Rn} -= {Rm};"
    if nm == "subc":
        return (f"{{ uint32_t a = {Rn}, b = {Rm}, s = a - b, r = s - {T}; "
                f"{T} = (a < b) | (s < r); {Rn} = r; }}")
    if nm == "subv":
        return (f"{{ int32_t a = (int32_t){Rn}, b = (int32_t){Rm}; int32_t r; "
                f"{T} = __builtin_sub_overflow(a, b, &r); {Rn} = (uint32_t)r; }}")
    if nm == "neg":
        return f"{Rn} = 0u - {Rm};"
    if nm == "negc":
        return (f"{{ uint32_t t = 0u - {Rm}; uint32_t r = t - {T}; "
                f"{T} = (0u < t) | (t < r); {Rn} = r; }}")
    if nm == "dt":
        return f"{Rn}--; {T} = ({Rn} == 0);"
    if nm == "cmp/eq":
        return f"{T} = ({Rn} == {Rm});"
    if nm == "cmp/eq #imm":
        return f"{T} = ({R0} == {s8(i) & 0xFFFFFFFF}u);"
    if nm == "cmp/hs":
        return f"{T} = ({Rn} >= {Rm});"
    if nm == "cmp/hi":
        return f"{T} = ({Rn} > {Rm});"
    if nm == "cmp/ge":
        return f"{T} = ((int32_t){Rn} >= (int32_t){Rm});"
    if nm == "cmp/gt":
        return f"{T} = ((int32_t){Rn} > (int32_t){Rm});"
    if nm == "cmp/pz":
        return f"{T} = ((int32_t){Rn} >= 0);"
    if nm == "cmp/pl":
        return f"{T} = ((int32_t){Rn} > 0);"
    if nm == "cmp/str":
        return (f"{{ uint32_t x = {Rn} ^ {Rm}; {T} = !(x & 0xFF) || !(x & 0xFF00) || "
                f"!(x & 0xFF0000) || !(x & 0xFF000000u); }}")
    if nm == "div0s":
        return f"g.q = {Rn} >> 31; g.m = {Rm} >> 31; {T} = g.q ^ g.m;"
    if nm == "div0u":
        return f"g.q = g.m = {T} = 0;"
    if nm == "div1":
        return f"sh4_div1({n}, {m});"
    if nm == "dmuls.l":
        return (f"{{ int64_t r = (int64_t)(int32_t){Rn} * (int32_t){Rm}; "
                f"g.mach = (uint32_t)((uint64_t)r >> 32); g.macl = (uint32_t)r; }}")
    if nm == "dmulu.l":
        return (f"{{ uint64_t r = (uint64_t){Rn} * {Rm}; "
                f"g.mach = (uint32_t)(r >> 32); g.macl = (uint32_t)r; }}")
    if nm == "mul.l":
        return f"g.macl = {Rn} * {Rm};"
    if nm == "muls.w":
        return f"g.macl = (uint32_t)((int32_t)(int16_t){Rn} * (int32_t)(int16_t){Rm});"
    if nm == "mulu.w":
        return f"g.macl = (uint32_t)(uint16_t){Rn} * (uint16_t){Rm};"
    if nm == "mac.l":
        return f"sh4_macl({n}, {m});"
    if nm == "mac.w":
        return f"sh4_macw({n}, {m});"
    if nm == "exts.b":
        return f"{Rn} = (uint32_t)(int8_t){Rm};"
    if nm == "exts.w":
        return f"{Rn} = (uint32_t)(int16_t){Rm};"
    if nm == "extu.b":
        return f"{Rn} = {Rm} & 0xFF;"
    if nm == "extu.w":
        return f"{Rn} = {Rm} & 0xFFFF;"

    # ---- logic ----
    for op, sym in (("and", "&"), ("or", "|"), ("xor", "^")):
        if nm == op:
            return f"{Rn} {sym}= {Rm};"
        if nm == op + " #imm":
            return f"{R0} {sym}= {i}u;"
        if nm == op + ".b #imm":
            return f"{{ uint32_t a = g.gbr + {R0}; W8(a, R8(a) {sym} {i}u); }}"
    if nm == "not":
        return f"{Rn} = ~{Rm};"
    if nm == "tst":
        return f"{T} = (({Rn} & {Rm}) == 0);"
    if nm == "tst #imm":
        return f"{T} = (({R0} & {i}u) == 0);"
    if nm == "tst.b #imm":
        return f"{T} = ((R8(g.gbr + {R0}) & {i}u) == 0);"
    if nm == "tas.b":
        return f"{{ uint32_t v = R8({Rn}); {T} = (v == 0); W8({Rn}, v | 0x80); }}"

    # ---- shifts ----
    if nm == "rotl":
        return f"{T} = {Rn} >> 31; {Rn} = ({Rn} << 1) | {T};"
    if nm == "rotr":
        return f"{T} = {Rn} & 1; {Rn} = ({Rn} >> 1) | ({T} << 31);"
    if nm == "rotcl":
        return f"{{ uint32_t t = {Rn} >> 31; {Rn} = ({Rn} << 1) | {T}; {T} = t; }}"
    if nm == "rotcr":
        return f"{{ uint32_t t = {Rn} & 1; {Rn} = ({Rn} >> 1) | ({T} << 31); {T} = t; }}"
    if nm in ("shll", "shal"):
        return f"{T} = {Rn} >> 31; {Rn} <<= 1;"
    if nm == "shlr":
        return f"{T} = {Rn} & 1; {Rn} >>= 1;"
    if nm == "shar":
        return f"{T} = {Rn} & 1; {Rn} = (uint32_t)((int32_t){Rn} >> 1);"
    for k in (2, 8, 16):
        if nm == f"shll{k}":
            return f"{Rn} <<= {k};"
        if nm == f"shlr{k}":
            return f"{Rn} >>= {k};"
    if nm == "shad":
        return (f"{{ int32_t s = (int32_t){Rm}; if (s >= 0) {Rn} <<= (s & 31); "
                f"else if ((s & 31) == 0) {Rn} = ((int32_t){Rn} < 0) ? 0xFFFFFFFFu : 0; "
                f"else {Rn} = (uint32_t)((int32_t){Rn} >> ((~s & 31) + 1)); }}")
    if nm == "shld":
        return (f"{{ int32_t s = (int32_t){Rm}; if (s >= 0) {Rn} <<= (s & 31); "
                f"else if ((s & 31) == 0) {Rn} = 0; else {Rn} >>= ((~s & 31) + 1); }}")

    # ---- flags / misc ----
    if nm == "clrt":
        return f"{T} = 0;"
    if nm == "sett":
        return f"{T} = 1;"
    if nm == "clrs":
        return "g.s = 0;"
    if nm == "sets":
        return "g.s = 1;"
    if nm == "clrmac":
        return "g.mach = g.macl = 0;"
    if nm in ("nop", "ocbi", "ocbp", "ocbwb", "ldtlb"):
        return ";"
    if nm == "pref":
        return f"sh4_pref({Rn});"
    if nm == "movca.l":
        return f"W32({Rn}, {R0});"
    if nm == "sleep":
        return "sh4_sleep();"
    if nm == "trapa":
        return f"sh4_trapa({i});"

    # ---- system registers ----
    simple = {"mach": "g.mach", "macl": "g.macl", "pr": "g.pr", "gbr": "g.gbr",
              "vbr": "g.vbr", "ssr": "g.ssr", "spc": "g.spc", "sgr": "g.sgr",
              "dbr": "g.dbr", "fpul": "g.fpul.u"}
    parts = nm.split(" ")
    if len(parts) == 2 and parts[0] in ("sts", "stc", "lds", "ldc", "sts.l", "stc.l",
                                        "lds.l", "ldc.l"):
        op, reg = parts
        if reg == "rbank":
            lhs = f"g.rbank[{m}]"
        elif reg == "sr":
            lhs = None
        elif reg == "fpscr":
            lhs = None
        else:
            lhs = simple[reg]
        get = {"sr": "sh4_get_sr()", "fpscr": "g.fpscr"}.get(reg, lhs)
        if op in ("sts", "stc"):
            return f"{Rn} = {get};"
        if op in ("sts.l", "stc.l"):
            return f"{{ uint32_t a = {Rn} - 4; W32(a, {get}); {Rn} = a; }}"
        if op in ("lds", "ldc"):
            src = Rn  # encoding uses the n field for the source register
            if reg == "sr":
                return f"sh4_set_sr({src});"
            if reg == "fpscr":
                return f"sh4_set_fpscr({src});"
            return f"{lhs} = {src};"
        if op in ("lds.l", "ldc.l"):
            if reg == "sr":
                return f"{{ uint32_t v = R32({Rn}); {Rn} += 4; sh4_set_sr(v); }}"
            if reg == "fpscr":
                return f"{{ uint32_t v = R32({Rn}); {Rn} += 4; sh4_set_fpscr(v); }}"
            return f"{lhs} = R32({Rn}); {Rn} += 4;"

    # ---- FPU ----
    F = lambda k: f"g.fr[{k}].f"
    Fu = lambda k: f"g.fr[{k}].u"
    if nm == "fmov":
        return f"FMOV_RR({n}, {m});"
    if nm == "fmov.s ld":
        return f"FMOV_LD({n}, {Rm});"
    if nm == "fmov.s ldinc":
        return f"{{ uint32_t a = {Rm}; {Rm} += FSZ(); FMOV_LD({n}, a); }}"
    if nm == "fmov.s r0ld":
        return f"FMOV_LD({n}, {Rm} + {R0});"
    if nm == "fmov.s st":
        return f"FMOV_ST({Rn}, {m});"
    if nm == "fmov.s stdec":
        return f"{{ uint32_t a = {Rn} - FSZ(); FMOV_ST(a, {m}); {Rn} = a; }}"
    if nm == "fmov.s r0st":
        return f"FMOV_ST({Rn} + {R0}, {m});"
    for op, sym in (("fadd", "+"), ("fsub", "-"), ("fmul", "*"), ("fdiv", "/")):
        if nm == op:
            return (f"if (FPR()) DR_SET({n}, DR({n}) {sym} DR({m})); "
                    f"else {F(n)} = {F(n)} {sym} {F(m)};")
    if nm == "fcmp/eq":
        return f"{T} = FPR() ? (DR({n}) == DR({m})) : ({F(n)} == {F(m)});"
    if nm == "fcmp/gt":
        return f"{T} = FPR() ? (DR({n}) > DR({m})) : ({F(n)} > {F(m)});"
    if nm == "fmac":
        return f"{F(n)} = fmaf({F(0)}, {F(m)}, {F(n)});"
    if nm == "fsts":
        return f"{Fu(n)} = g.fpul.u;"
    if nm == "flds":
        return f"g.fpul.u = {Fu(n)};"
    if nm == "float":
        return f"if (FPR()) DR_SET({n}, (double)g.fpul.i); else {F(n)} = (float)g.fpul.i;"
    if nm == "ftrc":
        return f"g.fpul.i = FPR() ? sh4_ftrc_d(DR({n})) : sh4_ftrc_f({F(n)});"
    if nm == "fneg":
        return f"{Fu(n)} ^= 0x80000000u;"
    if nm == "fabs":
        return f"{Fu(n)} &= 0x7FFFFFFFu;"
    if nm == "fsqrt":
        return f"if (FPR()) DR_SET({n}, sqrt(DR({n}))); else {F(n)} = sqrtf({F(n)});"
    if nm == "fsrra":
        return f"{F(n)} = 1.0f / sqrtf({F(n)});"
    if nm == "fldi0":
        return f"{Fu(n)} = 0;"
    if nm == "fldi1":
        return f"{Fu(n)} = 0x3F800000u;"
    if nm == "fcnvsd":
        return f"DR_SET({n}, (double)g.fpul.f);"
    if nm == "fcnvds":
        return f"g.fpul.f = (float)DR({n});"
    if nm == "fipr":
        a, b = (ins.op >> 10 & 3) * 4, (ins.op >> 8 & 3) * 4
        return f"sh4_fipr({b}, {a});"
    if nm == "ftrv":
        return f"sh4_ftrv({(ins.op >> 10 & 3) * 4});"
    if nm == "fsca":
        return f"sh4_fsca({(ins.op >> 9 & 7) * 2});"
    if nm == "frchg":
        return "sh4_set_fpscr(g.fpscr ^ FPSCR_FR);"
    if nm == "fschg":
        return "g.fpscr ^= FPSCR_SZ;"
    raise Unsupported(f"{nm} at 0x{pc:08X}")


def gen_function(fn, img, func_names, fsuffix=""):
    """C source for one function. func_names: entry addr -> C name, for direct calls."""
    out = []
    addrs = sorted(fn.insns)
    labels = set(fn.labels)
    # gather branch targets so we only emit labels that are used
    used = set()
    ds_addrs = set()
    for pc in addrs:
        ins = fn.insns[pc]
        if ins.has_delay_slot:
            ds_addrs.add(pc + 2)
        if ins.flow in (FLOW_BRANCH, FLOW_BRANCH_DS):
            used.add(branch_target(ins, pc))
            if ins.flow == FLOW_BRANCH_DS:
                used.add(pc + 4)
        elif ins.flow == FLOW_JUMP and branch_target(ins, pc) not in fn.tailcalls:
            used.add(branch_target(ins, pc))
        elif pc in fn.jumptables:
            used.update(fn.jumptables[pc])
    # delay slots that are also reached by other paths must be emitted as code
    _var_literals.clear()
    _var_literals.update(pcrel_addr(fn.insns[pc], pc) for pc in addrs if fn.insns[pc].name == "mova")
    name = func_names[fn.entry]
    out.append(f"void {name}(void) {{")
    if addrs[0] != fn.entry:
        # some blocks live below the entry point; start at the entry
        out.append(f"  goto L_{fn.entry:08X};")
        used.add(fn.entry)

    def call(target):
        c = func_names.get(target)
        return f"{c}();" if c else f"call_indirect(0x{target:08X}u, 0x{target:08X}u);"

    def goto(target, pc):
        chk = "IRQ_CHECK(); " if target <= pc else ""
        return f"{chk}goto L_{target:08X};"

    prev_falls = False
    for idx, pc in enumerate(addrs):
        ins = fn.insns[pc]
        is_ds_only = pc in ds_addrs and pc not in used and pc not in labels
        if is_ds_only:
            prev_falls = False
            continue
        if pc in used or (pc != fn.entry and pc in labels):
            out.append(f"L_{pc:08X}:")
        out.append(f"  /* {pc:08X} {ins.op:04X} {ins.name} */")
        prev_falls = True
        if ins.flow == 0 or ins.flow == 9:
            out.append("  " + emit_insn(ins, pc, img))
            continue
        ds = fn.insns.get(pc + 2)
        ds_c = emit_insn(ds, pc + 2, img) if ins.has_delay_slot else ""
        f = ins.flow
        if f == FLOW_BRANCH:
            cond = "g.t" if ins.name == "bt" else "!g.t"
            out.append(f"  if ({cond}) {{ {goto(branch_target(ins, pc), pc)} }}")
        elif f == FLOW_BRANCH_DS:
            cond = "c" if ins.name == "bt/s" else "!c"
            out.append(f"  {{ uint32_t c = g.t; {ds_c} if ({cond}) {{ {goto(branch_target(ins, pc), pc)} }} }}")
            out.append(f"  goto L_{pc + 4:08X};")
            prev_falls = False
        elif f == FLOW_JUMP:
            t = branch_target(ins, pc)
            if t in fn.tailcalls:
                out.append(f"  {ds_c} {call(t)} return;")
            else:
                out.append(f"  {ds_c} {goto(t, pc)}")
            prev_falls = False
        elif f == FLOW_CALL:
            t = branch_target(ins, pc)
            c = "SJLJ_SITE();" if t == SETJMP_ADDR else call(t)
            out.append(f"  g.pr = 0x{pc + 4:08X}u; {ds_c} {c}")
            out.append(f"  goto L_{pc + 4:08X};")
            used.add(pc + 4)
        elif f == FLOW_CALL_REG:
            if ins.name == "jsr":
                tgt = R(ins.n)
            else:  # bsrf
                tgt = f"0x{pc + 4:08X}u + {R(ins.n)}"
            out.append(f"  {{ uint32_t a = {tgt}; g.pr = 0x{pc + 4:08X}u; {ds_c} "
                       f"if (a == SETJMP_ADDR) SJLJ_SITE(); else call_indirect(a, 0x{pc:08X}u); }}")
            out.append(f"  goto L_{pc + 4:08X};")
            used.add(pc + 4)
        elif f == FLOW_JUMP_REG:
            if pc in fn.jumptables:
                cases = " ".join(f"case 0x{t:08X}u: goto L_{t:08X};"
                                 for t in sorted(set(fn.jumptables[pc])))
                out.append(f"  {{ uint32_t a = 0x{pc + 4:08X}u + {R(ins.n)}; {ds_c} "
                           f"switch (a) {{ {cases} default: bad_jump(a, 0x{pc:08X}u); }} }}")
            else:
                tgt = R(ins.n) if ins.name == "jmp" else f"0x{pc + 4:08X}u + {R(ins.n)}"
                out.append(f"  {{ uint32_t a = {tgt}; {ds_c} call_indirect(a, 0x{pc:08X}u); return; }}")
            prev_falls = False
        elif f == FLOW_RET:
            out.append(f"  {ds_c} RETURN();")
            prev_falls = False
        elif f == FLOW_RTE:
            out.append(f"  {ds_c} sh4_rte(); return;")
            prev_falls = False
    out.append("}")
    # labels for call returns: insert where missing (pc+4 after calls always exists as code)
    text = "\n".join(l for l in out if l != "")
    emitted = {int(l[2:10], 16) for l in out if l.startswith("L_")}
    missing = (used - emitted)
    if missing:
        # add labels before the comment line of those instructions
        lines = text.split("\n")
        res = []
        for l in lines:
            if l.startswith("  /* "):
                a = int(l[5:13], 16)
                if a in missing:
                    res.append(f"L_{a:08X}:")
                    missing.discard(a)
            res.append(l)
        text = "\n".join(res)
        if missing:
            # targets that don't exist as emitted code (e.g. fall off) -> stub out
            body_end = text.rfind("}")
            stubs = "".join(f"L_{a:08X}: bad_jump(0x{a:08X}u, 0); return;\n" for a in sorted(missing))
            text = text[:body_end] + stubs + "}"
    return text
