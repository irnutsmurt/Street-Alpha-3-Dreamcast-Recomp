"""Recompile the game's SH-4 code to C.

Outputs into <out_dir>:
  funcs.h          prototypes
  code_NNN.c       recompiled functions (chunked for parallel compilation)
  dispatch.c       guest address -> host function table
"""
import json
import os
import struct

from analyze import Image, canon, discover, has_prologue
from codegen import gen_function

MAIN_BASE = 0x8C010000
OVERLAY_BASE = 0x8C800000
OVERLAY_SLOT = 0x14000
CHUNK = 150
CHUNK_SPAN = 0x4000  # main-program functions are grouped by address range

# Guest functions replaced by native runtime implementations.
HOOKS = {
    0x8C0100C8: "hle_task_switch",  # cooperative task context switch (needs host fibers)
    0x8C094510: "hle_longjmp",      # longjmp (coroutines across stacks)
    0x8C0100BC: "hle_task_create",  # task frame setup (marks the task as freshly created)
    # Option Mode > DISPLAY: the CRT position/size screen becomes the PC display settings
    0x8C07BC42: "hle_display_logic",
    0x8C07C6B0: "hle_display_draw",
    # Option Mode > BUTTON CONFIG: the PC controls (real button names, rebinding, DEVICE)
    0x8C07B7B4: "hle_buttoncfg_logic",
    0x8C07C430: "hle_buttoncfg_draw",
    # ... and its copy in the pause menu during a fight
    0x8C07D2E0: "hle_pause_buttoncfg_draw",
    0x8C07CD5A: "hle_pause_buttoncfg_p1",
    0x8C07CE14: "hle_pause_buttoncfg_p2",
    # the title screen: Back / Esc ask QUIT GAME?
    0x8C0625E0: "hle_title_logic",
    # the pause menu during a fight: DISPLAY added
    0x8C02EB8C: "hle_pause_logic",
    0x8C02EE26: "hle_pause_draw",
    0x8C07E17C: "hle_option_object",  # Option Mode sprite objects (hides the old screen's widgets)
    # Option Mode's menu: a fifth button, CHEATS
    0x8C07AF3C: "hle_option_top",
    0x8C07D988: "hle_option_button",
    # Mode Menu > NETWORK: the online menu becomes the netplay lobby
    0x8C02D3E0: "hle_network_init",
    0x8C02D4C0: "hle_network_logic",
    0x8C06273C: "hle_mode_menu_input",  # Mode Menu input: start a mode directly (lobby, autopilot)
    0x8C01348E: "hle_pac_section",  # a character archive's next section: big sprite tiles to extension memory
    0x8C05206E: "hle_throw_a",  # throws: the PSP characters as the held opponent (psp_load.c)
    0x8C0520F0: "hle_throw_b",
    0x8C052180: "hle_throw_c",
    0x8C052268: "hle_throw_d",
    0x8C0253F8: "hle_fighter_reset",  # a fighter's reset for a round: the PSP-only fields too (psp.c)
    0x8C058516: "hle_ai_step",  # the CPU script interpreter: the PSP's command 80 (psp.c)
    0x8C06443C: "hle_hud",  # the fight HUD: the PSP characters' faces and names (psp_load.c)
    0x8C069D34: "hle_vs_palette",  # VS portraits' palettes: the PSP characters' (psp_load.c)
    0x8C069DCC: "hle_count_use",  # the use counters (saved): not for the PSP characters (psp_load.c)
    0x8C0126F4: "hle_open_numbered",  # opening a file by its names[] number: the PSP characters' (psp_load.c)
    0x8C012C80: "hle_open_file",    # opening a file by name (logged with SFA3_FILE_LOG)
    0x8C018E9C: "hle_sprite_draw",  # debug: SFA3_SPRITE_LOG (sprite_log.c)
    0x8C069D64: "hle_select_palette",  # select screen palettes: the PSP characters' portraits (psp_load.c)
    0x8C018084: "hle_layout_draw",  # layouts (select screen): the PSP characters' extra cells (psp_load.c)
}
# Hooked functions whose original body is still emitted, as <name>_orig.
KEEP_ORIGINAL = {0x8C0253F8, 0x8C058516, 0x8C06443C, 0x8C05206E, 0x8C0520F0, 0x8C052180, 0x8C052268, 0x8C069D34, 0x8C069DCC, 0x8C0126F4, 0x8C012C80, 0x8C018E9C, 0x8C069D64, 0x8C018084, 0x8C01348E, 0x8C07AF3C, 0x8C07D988, 0x8C0625E0, 0x8C02EB8C, 0x8C02EE26, 0x8C07B7B4, 0x8C07D2E0, 0x8C07CD5A, 0x8C07CE14, 0x8C094510, 0x8C0100BC, 0x8C07BC42, 0x8C07E17C, 0x8C06273C}


def extra_entries():
    """Entry points only reachable through computed jumps, found at runtime."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "extra_entries.txt")
    out = []
    if os.path.exists(path):
        for line in open(path):
            line = line.split("#")[0].strip()
            if line:
                out.append(canon(int(line, 16)))
    return out


def _chartables():
    import importlib.util
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "psp", "chartables.py")
    spec = importlib.util.spec_from_file_location("chartables", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return
    with open(path, "w") as f:
        f.write(text)


def recompile_image(img, entries, prefix, suffix=""):
    funcs = discover(img, entries)
    good = {a: f for a, f in funcs.items() if not f.bad}
    names = {a: f"f_{a:08X}{suffix}" for a in good}
    return good, names


def overlay_entries(img):
    """Entry candidates: the image start plus words pointing into the image."""
    ents = {img.base}
    for off in range(0, len(img.data) - 3, 4):
        v = struct.unpack_from("<I", img.data, off)[0]
        v = (v & 0x1FFFFFFF) | 0x80000000
        if img.contains(v) and not v & 1:
            ents.add(v)
    return sorted(ents)


def fn_extent(fn):
    hi = max(max(fn.insns) + 2, max(fn.pool) + 2 if fn.pool else 0)
    return min(hi - fn.entry, 4096) if hi > fn.entry else 2


def run(disc_dir, out_dir, report=None, progress=None):
    """report(text): what it is doing, as it goes (default: printed).
    progress(done, total): over the overlay files, then the main program."""
    report = report or (lambda m: print("      " + m))
    progress = progress or (lambda d, t: None)
    os.makedirs(out_dir, exist_ok=True)
    # the tables indexed by character number move to the extension RAM, with
    # room for the PSP characters (tools/psp/chartables.py): their literals follow
    import codegen
    chartables = _chartables()
    codegen.RELOCATE[:] = chartables.relocations()
    report(f"{len(codegen.RELOCATE)} character tables moved")
    data = open(os.path.join(disc_dir, "1ST_READ.BIN"), "rb").read()
    img = Image(data, MAIN_BASE)

    # --- code overlays: characters (PLnnA/B/C.BIN) and arcade endings (ENDnn.BIN) ---
    ovl_files = sorted(f for f in os.listdir(disc_dir)
                       if f.endswith(".BIN") and ((f.startswith("PL") and len(f) == 9)
                                                  or (f.startswith("END") and len(f) == 9)))
    ovl_funcs = []      # (addr, cname, sig bytes)
    main_refs = set()
    ovl_parts = []
    steps = len(ovl_files) + 3  # the overlays, the main program, writing the C, the sound driver
    for i, fname in enumerate(ovl_files):
        tag = fname[:-4]
        odata = open(os.path.join(disc_dir, fname), "rb").read()
        # PLnnA/B/C are linked for the player slots at 0x14000-byte intervals;
        # endings load at the start of the overlay area
        base = OVERLAY_BASE
        if fname.startswith("PL"):
            base += "ABC".index(fname[4]) * OVERLAY_SLOT
        oimg = Image(odata, base)
        funcs = discover(oimg, overlay_entries(oimg))
        good = {a: f for a, f in funcs.items() if not f.bad}
        names = {a: f"f_{a:08X}_{tag}" for a in good}
        for f in good.values():
            for t in f.calls | f.tailcalls:
                if img.contains(t):
                    main_refs.add(t)
        # Character code also calls main-program functions through addresses
        # kept in its data (callback tables, pointers loaded into registers).
        # Any word in the overlay that points at a function prologue in the
        # main program is treated as an entry point.
        for off in range(0, len(odata) - 3, 4):
            t = canon(struct.unpack_from("<I", odata, off)[0])
            if img.contains(t) and not t & 1 and has_prologue(img, t):
                main_refs.add(t)
        for a in sorted(good):
            f = good[a]
            ovl_parts.append((names[a], gen_function(f, oimg, names)))
            sig = odata[a - base:a - base + fn_extent(f)]
            ovl_funcs.append((a, names[a], sig))
        what = f"character {fname[2:4]} part {fname[4]}" if fname.startswith("PL") else f"ending {fname[3:5]}"
        report(f"{fname} ({what}): {len(good)} functions")
        progress(i + 1, steps)
    report(f"overlays: {len(ovl_files)} files, {len(ovl_funcs)} functions")
    report("1ST_READ.BIN (the main program): finding its functions...")

    # --- main program ---
    good, names = recompile_image(img, [MAIN_BASE, 0x8C128BD4] + sorted(main_refs) + extra_entries(),
                                  "main")
    report(f"1ST_READ.BIN: {len(good)} functions")
    progress(len(ovl_files) + 1, steps)
    report("writing the C code...")

    all_names = set(names.values()) | {c for c, _ in ovl_parts}
    protos = ["#pragma once", '#include "cpu.h"']
    protos += [f"void {n}(void);" for n in sorted(all_names)]
    protos += [f"void {h}(void);" for h in sorted(set(HOOKS.values()))]
    protos += [f"void {names[a]}_orig(void);" for a in sorted(KEEP_ORIGINAL)]
    write_if_changed(os.path.join(out_dir, "funcs.h"), "\n".join(protos) + "\n")

    entries = sorted(good)
    files = []
    groups = {}
    for a in entries:
        groups.setdefault((a - MAIN_BASE) // CHUNK_SPAN, []).append(a)
    for gi, addrs in sorted(groups.items()):
        parts = ['#include "funcs.h"', ""]
        for a in addrs:
            if a in HOOKS:
                parts.append(f"void {names[a]}(void) {{ {HOOKS[a]}(); }}")
                if a in KEEP_ORIGINAL:
                    orig = dict(names)
                    orig[a] = names[a] + "_orig"
                    parts.append(gen_function(good[a], img, orig))
            else:
                parts.append(gen_function(good[a], img, names))
            parts.append("")
        fn = f"code_{gi:04d}.c"
        write_if_changed(os.path.join(out_dir, fn), "\n".join(parts))
        files.append(fn)

    for ci in range(0, len(ovl_parts), CHUNK):
        parts = ['#include "funcs.h"', ""]
        for cname, text in ovl_parts[ci:ci + CHUNK]:
            parts.append(text)
            parts.append("")
        fn = f"ovl_{ci // CHUNK:03d}.c"
        write_if_changed(os.path.join(out_dir, fn), "\n".join(parts))
        files.append(fn)

    disp = ['#include "funcs.h"', '#include "dispatch.h"', '#include "overlay.h"',
            "const DispatchEntry g_dispatch_main[] = {"]
    disp += [f"  {{0x{a:08X}u, {names[a]}}}," for a in entries]
    disp += ["};", f"const unsigned g_dispatch_main_count = {len(entries)};", ""]
    sigs = {}
    for a, cname, sig in ovl_funcs:
        sigs.setdefault(sig, f"s_sig{len(sigs)}")
    for sig, sname in sigs.items():
        body = ",".join(str(b) for b in sig)
        disp.append(f"static const uint8_t {sname}[] = {{{body}}};")
    disp.append("const OverlayEntry g_overlay_funcs[] = {")
    ovl_funcs.sort()
    for a, cname, sig in ovl_funcs:
        disp.append(f"  {{0x{a:08X}u, {cname}, {len(sig)}, {sigs[sig]}}},")
    disp += ["};", f"const unsigned g_overlay_count = {len(ovl_funcs)};", ""]
    write_if_changed(os.path.join(out_dir, "dispatch.c"), "\n".join(disp))
    files.append("dispatch.c")
    # where they went, for the runtime to copy them at boot (runtime/chartables.c)
    ct = ['#include "chartables.h"', "const CharTable g_char_tables[] = {"]
    ct += [f"  {{0x{t.dc:08X}u, {t.copy_count}u, {t.size}u, 0x{t.ext:08X}u, {t.entries}u}},  /* {t.note} */"
           for t in chartables.TABLES]
    ct += ["};", f"const unsigned g_char_table_count = {len(chartables.TABLES)};", ""]
    write_if_changed(os.path.join(out_dir, "char_tables.c"), "\n".join(ct))
    files.append("char_tables.c")

    report(f"wrote {len(files)} C files")
    progress(len(ovl_files) + 2, steps)
    # ARM7 sound driver
    import arm
    drv = open(os.path.join(disc_dir, "MANATEE.DRV"), "rb").read()
    arm_c, n_arm = arm.generate(drv)
    write_if_changed(os.path.join(out_dir, "arm_code.c"), arm_c)
    files.append("arm_code.c")
    report(f"MANATEE.DRV (the sound driver): {n_arm} ARM instructions")
    progress(steps, steps)
    write_if_changed(os.path.join(out_dir, "sources.txt"), "\n".join(files) + "\n")
