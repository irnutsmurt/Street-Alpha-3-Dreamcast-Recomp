"""The Dreamcast game's tables indexed by character number, made room in for
the PSP characters (0x26-0x29).

The Dreamcast has 38 characters (0x00-0x25) and its tables have 38 entries
(a few 40), packed one after another, so they can't grow where they are.
Every table listed here is moved: the recompiler rewrites the literals that
point into it (codegen.relocate) to its new place in the extension RAM
(EXT), sized for 42 characters. At boot the runtime (runtime/chartables.c)
copies the Dreamcast's entries there and fills the new ones with copies of
Ryu's (entry 0); with the add-on built, build_char.py supplies the PSP's
entries for 0x26-0x29 instead, converted (patches()).

The PSP is the same game with the four characters added, so its tables are
the Dreamcast's with four more entries: each table's PSP address is listed
and checked against the Dreamcast's entries when the add-on is built. How the
new entries are converted is the table's kind:
  copy      the bytes as they are (numbers: sizes, speeds, flags, ...)
  ptr       u32 pointers to records the Dreamcast table also points at: the
            PSP record -> the Dreamcast record the same characters use
  ptr_list  u32 pointers to lists of u16 ending in 0xFFFF: a list the
            Dreamcast has is reused, a new one is copied into EXT

Only tables the game never writes are moved (the literals are rewritten in
loads and stores alike, but a table that is saved or copied as part of a
bigger block would lose its writes). docs/psp-characters.md has the list
with what each table is for, as far as known.

    python3 tools/psp/chartables.py <Dreamcast 1ST_READ.BIN> <PSP BOOT.BIN> [generated C dir]
checks every table against both programs (and the literals that point at
it, with the generated C).
"""
import os
import re
import struct
import sys

EXT = 0x98D00000         # the moved tables
EXT_DATA = 0x98E00000    # records the PSP's new entries point at (patches())
EXT_THROWS = 0x98E20000  # throw positions for the PSP characters as opponents (throws.py)
EXT_NAMES = 0x98E30000   # the add-on's file names (file_patches())
EXT_ROUTES = 0x98E31000  # arcade routes (arcade.py)
EXT_SELECT = 0x98E40000  # the select screens' records (screens.py)
EXT_RANKING = 0x98E70000  # the ranking portraits' animations (ranking.py)
EXT_END = 0x98E80000     # (build_char.py: the PSP-only variables from here)
EXT_IMM = 0x98DFF000     # constants of the game's code the add-on can change (IMMEDIATES)

# Instructions whose constant (mov #imm, add #imm, cmp/eq #imm, mov.w/mov.l @(disp,PC)) the recompiled code reads
# from EXT_IMM + 4 x index instead: the runtime writes the instructions' own values there at boot
# (runtime/chartables.c) and the add-on's patches change some (screens.py: the versus grid's 7th row)
IMMEDIATES = [
    (0x8C0706E4, "versus select cursor: the last row (5)"),
    (0x8C0703E6, "versus select: the World Tour entries' row (cmp/eq #5, f_8C0703CC)"),
    (0x8C06F0BE, "versus select: the World Tour entries' row in the grid (35: row 5)"),
    (0x8C06F138, "the same"),
    (0x8C06F162, "the same"),
    (0x8C06F85A, "versus select: the SPEED line's y (0xAE)"),
    (0x8C06F868, "the same, its value"),
    (0x8C06F892, "versus select: the STAGE line's y (0xBE)"),
    (0x8C06F8A4, "the same, its value"),
    (0x8C01C520, "World Tour: room for bank 0x1D's tiles (WORLD.PAC), malloc'd (0x30000; f_8C01C508)"),
    (0x8C01C600, "the same (f_8C01C5E8)"),
]


def immediate(address):
    """the place in the extension RAM of an instruction's constant (IMMEDIATES)"""
    for k, (a, _) in enumerate(IMMEDIATES):
        if a == address:
            return EXT_IMM + 4 * k
    raise KeyError(hex(address))
CHARS = 0x2A             # entries after the move: 0x00-0x29
NEW = range(0x26, 0x2A)
PSP_OFF = 0x74           # BOOT.BIN: file offset = address + 0x74
MAIN = 0x8C010000


# the file name table: names[] index -> name. A character's files are numbered
# by family (pl 13 + n, rk 90 + n, op 126 + n; n the character number, one
# less above 0x12), which for 0x26 and up runs into the next family; win, bd
# and end go through tables. The add-on's names follow the Dreamcast's 271:
# NAME_COUNT + 4 x family + (character - 0x26), the families in FAMILIES
# order (runtime/psp_load.c has the same numbers).
NAMES = 0x8C09895C
NAME_COUNT = 271
FAMILIES = ("pl", "op", "rk", "win", "bd", "end")
FAMILY_TABLES = {"win": 0x8C09B08E, "bd": 0x8C09B116, "end": 0x8C09AC12}  # u16 names[] index per character


def name_index(family, c):
    return NAME_COUNT + 4 * FAMILIES.index(family) + (c - NEW.start)


class T:
    """a table: the Dreamcast's at dc (count entries of size bytes), the PSP's at
    psp; moved with room for `entries` entries, the PSP's entries `new` added"""
    def __init__(self, dc, size, psp, kind="copy", count=38, note="", entries=CHARS, new=NEW):
        self.dc, self.size, self.psp, self.kind, self.count, self.note = dc, size, psp, kind, count, note
        self.entries, self.new = entries, new
        self.ext = None
        # entries copied from the Dreamcast: at least 38 for a character table,
        # so one a short (the next object's first bytes read as the last
        # character's) keeps reading what the Dreamcast did
        self.copy_count = max(count, 38) if entries == CHARS else count

    @property
    def end(self):
        return self.dc + self.size * self.count


TABLES = [
    # --- fighting: the engine's per-character numbers ---
    T(0x8C0B2088, 4, 0x2154E0, note="f_8C04BF86"),
    T(0x8C0B21BC, 4, 0x215632, note="per -ism bytes (+1 + ism) -> fighter +0x231 (f_8C04EB60)"),
    T(0x8C0B2254, 4, 0x2156DA, note="f_8C04EB76"),
    T(0x8C0B57B8, 1, 0x219B88, note="f_8C0546D6"),
    T(0x8C0B57DE, 1, 0x219BB2, note="f_8C0546D6"),
    T(0x8C0B7C08, 4, 0x21C02C, "ptr", note="f_8C05090C"),
    T(0x8C0B7CC0, 4, 0x21C0F4, "ptr", note="f_8C050982"),
    T(0x8C0B7D58, 1, 0x21C19C, note="f_8C04E900"),
    T(0x8C0B7D7E, 4, 0x21C1C6, note="-ism 0: fighter +0x2A0 points here (f_8C04E900); 2 u16"),
    T(0x8C0B7E16, 4, 0x21C26E, note="-ism > 0: fighter +0x2A0"),
    T(0x8C0B7EAE, 4, 0x21C316, note="-ism < 0: fighter +0x2A0"),
    T(0x8C0B7F46, 32, 0x21C3BE, note="-ism 0: fighter +0x2A4 points here (f_8C04E900)"),
    T(0x8C0B8406, 32, 0x21C8FE, note="-ism > 0: fighter +0x2A4"),
    T(0x8C0B88C6, 32, 0x21CE3E, note="-ism < 0: fighter +0x2A4"),
    T(0x8C0B8DA8, 1, 0x21D3A0, note="f_8C04E900"),
    T(0x8C0B8DCE, 1, 0x21D3CA, note="f_8C04E900, f_8C03CFEA"),
    T(0x8C0B8DF4, 1, 0x21D3F4, note="f_8C049A86"),
    T(0x8C0B8E1A, 1, 0x21D41E, note="f_8C049D28"),
    T(0x8C0B9A74, 1, 0x21E0D0, note="f_8C056B4C"),
    T(0x8C0BA66C, 1, 0x21ED30, note="f_8C05A010 (sound)"),
    T(0x8C0BBD44, 2, 0x220408, count=37, note="f_8C05B05E; 37 entries (0x8C0BBD8E is the next object)"),
    T(0x8C0BC0BC, 4, 0x220798, note="f_8C05C01C; 2 u16"),
    T(0x8C0BC5D8, 2, 0x220C80, count=40, note="f_8C05D732"),
    T(0x8C0C0E54, 1, 0x2260EC, note="f_8C06AA2C: a character number"),
    T(0x8C0C15A0, 1, 0x226BC8, note="f_8C06D1C8"),
    T(0x8C0C1B14, 2, 0x2270B0, note="f_8C06DC3C"),
    T(0x8C0D5498, 1, 0x239348, note="f_8C0904D6"),
    T(0x8C0B4DF8, 32, 0x2190C8, note="f_8C055460: a row of 32 bytes per character, indexed by fighter +0x157"),
    # --- files: names[] indices (the file name table 0x8C09895C); the add-on's files get their own ---
    T(NAMES, 4, None, count=NAME_COUNT, entries=NAME_COUNT + 4 * len(FAMILIES), new=(),
      note="file names (f_8C0126F4 and the openers); the add-on's after the Dreamcast's"),
    T(0x8C09B08E, 2, None, count=40, note="win screen file (f_8C01C2A6); 0x24/0x25 are win24/win25"),
    T(0x8C09B0DE, 1, None, count=40, note="win screen: a byte per character (f_8C01C2A6)"),
    T(0x8C09B116, 2, None, count=40, note="bd file (f_8C01CA18): win portrait sprites"),
    T(0x8C09AC12, 2, None, count=40, note="ending file (f_8C01BA82, f_8C01BC2A); 0: none"),
    # --- arcade mode: the route, the world map, stages ---
    T(0x8C0A6DF8, 1, 0x20B1C4, note="f_8C02B774"),
    T(0x8C0A70AE, 1, 0x20B3A8, note="f_8C02C434 (a lookup function): the PSP's new entries are stages"),
    T(0x8C0A70D6, 1, 0x20B3D2, note="f_8C02C43A: a character number (0x13 -> 0x11)"),
    T(0x8C0A8248, 4, None, note="the routes: 16 x 8 opponents (f_8C02BD08; arcade.py makes the PSP's)"),
    T(0x8C0A7176, 1, 0x20A784, note="the route's 9th opponent, the rival (f_8C02BD08)"),
    T(0x8C0A719E, 1, 0x20A7AE, note="the route's last opponent, the boss (f_8C02BD08, f_8C02B8CE)"),
    T(0x8C0A8380, 1, 0x20B64C, note="f_8C02C758"),
    T(0x8C0A83A8, 1, 0x20B676, note="f_8C02C758 and on: a flag per character"),
    T(0x8C0A83D0, 1, 0x20B6A0, note="a flag per character"),
    T(0x8C0A83F8, 1, 0x20B6CA, note="a flag per character"),
    T(0x8C0A8420, 1, 0x20B604, note="the character's stage (f_8C02CA78; PSP 0x2D798); 0xFF: none"),
    T(0x8C0CF7D4, 1, 0x234154, note="prologue: the portrait's first part in the OP file's bank 0x15 (f_8C07E584)"),
    T(0x8C0C1350, 1, 0x20A7AE, note="the boss again (f_8C06C02C & co.; the PSP's is the route's table)"),
    # --- the select screens (tools/psp/screens.py fills the new entries) ---
    T(0x8C0BE434, 1, 0x222FF6, note="select (f_8C06920E)"),
    T(0x8C0BE266, 1, 0x222D4C, note="f_8C06443C"),
    T(0x8C0BED8C, 4, None, count=30, entries=30, new=(),
      note="animations by kind of object: a pointer per kind to one of two record groups (screens.py)"),
    T(0x8C0C0C60, 4, None, count=15, entries=15, new=(),
      note="the fight HUD's animations by kind: the 5th, 8C0C0AA6, the faces (screens.py)"),
    T(0x8C0C2140, 2, 0x227858, note="f_8C070320"),
    T(0x8C0AB820, 2, None, note="the names again: bank 8 parts (f_8C036730)"),
    T(0x8C0C0D26, 2, None, note="the names again (f_8C06A5A8)"),
    T(0x8C0C03B4, 4, None, count=40, note="select: big portrait animation (f_8C06BAD8)"),
    T(0x8C0C1194, 2, None, note="select: name, a bank 8 part (f_8C06B272)"),
    T(0x8C0C1AC8, 2, None, note="the world map's name strip: a bank 8 part (f_8C06DC3C; spacing 8C0C1B14)"),
    T(0x8C0C11F4, 4, None, note="select: the name strip down the side: u16 spacing (0x8000: name 8 up), u16 bank 8 part (f_8C06B488)"),
    T(0x8C0C142C, 2, None, note="select: name banner, a bank 8 part (f_8C06C866)"),
    T(0x8C0BE45A, 2, None, note="a character's face: bank B part (f_8C067E34, f_8C06EFAC; versus' World Tour entries)"),
    T(0x8C0AB7D0, 2, None, note="World Tour: the character's banner, the first of 3 bank 0x1D parts (WORLD.PAC; f_8C0366D0)"),
    T(0x8C0C1478, 2, None, note="select: name banner width (f_8C06C866)"),
    # --- not by character: the PSP's additions the new characters use ---
    T(0x8C0B39C8, 4, 0x2178C8, "cmd", count=64, entries=66, new=range(64, 66),
      note="command definitions (f_8C04EBDC): 0x40 QCB x2 ending back + K, 0x41 QCF x2 ending forward + K"),
    # object handlers by type, filled as a fight starts from each character's descriptor lists of
    # (type, handler) (f_8C046D6E). The PSP's tables are longer: the new characters' types run past
    # the Dreamcast's ends, whose next tables they'd overwrite (Ingrid's word 28 types 45 and 46 are
    # the stage objects' handlers 2 and 3 in 8C0A93E0)
    T(0x8C0A9278, 4, None, count=47, entries=59, new=(),
      note="descriptor word 29's handlers; new types 47-58 (Maki 47-49, Yun 50 56 57, Eagle 51 52, Ingrid 53-55 58)"),
    T(0x8C0A9334, 4, None, count=43, entries=47, new=(),
      note="descriptor word 28's handlers; new types 43-46 (Eagle 43, Yun 44, Ingrid 45 46)"),
    # --- the menus (tools/psp/menus.py fills the new entries) ---
    T(0x8C0D2B01, 1, None, count=37, note="score ranking: a character's face, a part of MSEL.PAC's bank (8C082578)"),
    T(0x8C0D3045, 1, None, count=37, note="the same, another menu module's copy (8C083E40)"),
    T(0x8C0D3D1D, 1, None, count=37, note="the same, another menu module's copy (8C08AEAC and on)"),
    T(0x8C0D2AB2, 2, None, count=37, note="score ranking: the face mirrored (u16; its low byte to the object's +0xB)"),
    T(0x8C0D2FF6, 2, None, count=37, note="the same, the second module's copy"),
    T(0x8C0D3CCA, 2, None, count=37, note="the same, the third module's copy"),
    # --- the ranking's portraits (tools/psp/ranking.py fills the new entries) ---
    T(0x8C0D2035, 1, None, count=37, note="the ranking portrait screen: the name plate, RK_COM.PAC part this + 2 (8C07EEBA)"),
    T(0x8C0D1EA8, 4, None, count=38, note="the ranking portrait screen: an animation (8-byte records, RKnn.PAC parts; 8C07F130)"),
    T(0x8C0D1F40, 4, None, count=38, note="the same, another animation (8C07F16E)"),
]


def layout():
    a = EXT
    for t in TABLES:
        t.ext = a
        a += (t.size * t.entries + 3) & ~3
    assert a <= EXT_DATA, "the moved tables outgrew their room"
    return TABLES


def relocations():
    """(start, end, new start) for the recompiler"""
    return [(t.dc, t.end, t.ext) for t in layout()]


# ---------------- the add-on: the PSP's entries ----------------

class Programs:
    def __init__(self, dc_main, psp_boot):
        self.dc = open(dc_main, "rb").read() if isinstance(dc_main, str) else dc_main
        self.psp = open(psp_boot, "rb").read() if isinstance(psp_boot, str) else psp_boot

    def d(self, a, n):
        return self.dc[a - MAIN:a - MAIN + n]

    def p(self, a, n):
        return self.psp[a + PSP_OFF:a + PSP_OFF + n]

    def du32(self, a):
        return struct.unpack_from("<I", self.dc, a - MAIN)[0]

    def pu32(self, a):
        return struct.unpack_from("<I", self.psp, a + PSP_OFF)[0]


def cmd_record(read, a):
    """a command definition: u16 directions, the last with 0x400 set, then the buttons"""
    out = bytearray()
    while len(out) < 64:
        h = read(a, 2)
        out += h
        a += 2
        if int.from_bytes(h, "little") & 0x400:
            return bytes(out + read(a, 2))
    raise ValueError(f"command definition at {a:X}: no end")


def agreement(pr, t):
    """how many of the Dreamcast's entries (38 at most) the PSP table has the same"""
    n = min(t.count, 38)
    if t.kind == "copy":
        return sum(pr.d(t.dc + t.size * k, t.size) == pr.p(t.psp + t.size * k, t.size) for k in range(n))
    if t.kind == "cmd":  # the records the same
        return sum(cmd_record(pr.d, pr.du32(t.dc + 4 * k)) == cmd_record(pr.p, pr.pu32(t.psp + 4 * k))
                   for k in range(n))
    # pointers: the same pattern of shared records
    d = [pr.du32(t.dc + 4 * k) for k in range(n)]
    p = [pr.pu32(t.psp + 4 * k) for k in range(n)]
    return sum(all((d[i] == d[j]) == (p[i] == p[j]) for j in range(n)) for i in range(n))


def patches(pr):
    """[(address, bytes)]: the new characters' entries (and the records they need)"""
    layout()
    out, data = [], EXT_DATA
    for t in TABLES:
        if t.psp is None:
            continue
        ok, n = agreement(pr, t), min(t.count, 38)
        if ok < n * 4 // 5:
            raise ValueError(f"table {t.dc:08X}: the PSP's at {t.psp:06X} matches only {ok}/{n} entries")
        for c in t.new:
            src = t.psp + t.size * c
            if t.kind == "copy":
                v = pr.p(src, t.size)
            elif t.kind == "cmd":
                rec = cmd_record(pr.p, pr.pu32(src))
                out.append((data, rec))
                v = struct.pack("<I", data)
                data = (data + len(rec) + 3) & ~3
            elif t.kind in ("ptr", "ptr_list"):
                target = pr.pu32(src)
                pairs = {pr.pu32(t.psp + 4 * k): pr.du32(t.dc + 4 * k) for k in range(38)}
                if target in pairs:
                    dc_target = pairs[target]
                elif t.kind == "ptr_list":
                    rec = bytearray()
                    a = target
                    while True:
                        h = pr.p(a, 2)
                        rec += h
                        a += 2
                        if h == b"\xff\xff":
                            break
                    out.append((data, bytes(rec)))
                    dc_target, data = data, (data + len(rec) + 3) & ~3
                else:
                    raise ValueError(f"table {t.dc:08X}: character {c:02X}'s record {target:06X} is new")
                v = struct.pack("<I", dc_target)
            else:
                raise ValueError(t.kind)
            out.append((t.ext + t.size * c, v))
    assert data <= EXT_THROWS
    return out


def file_patches(families):
    """[(address, bytes)]: the add-on's file names (FAMILIES' files that it
    has built, for the new characters) and the tables that pick them"""
    tables = {t.dc: t for t in layout()}
    names = tables[NAMES]
    out, a = [], EXT_NAMES
    for f in families:
        for c in NEW:
            s = f"{f}{c:02x}.pac".encode() + b"\0"
            out.append((a, s))
            out.append((names.ext + 4 * name_index(f, c), struct.pack("<I", a)))
            a += (len(s) + 3) & ~3
            if f in FAMILY_TABLES:
                out.append((tables[FAMILY_TABLES[f]].ext + 2 * c, struct.pack("<H", name_index(f, c))))
    assert a <= EXT_ROUTES
    return out


def check(dc_main, psp_boot, gen_dir=None):
    pr = Programs(dc_main, psp_boot)
    layout()
    lits = {}
    if gen_dir:
        pat = re.compile(r"= 0x(8C0[0-9A-F]{5})u;")
        for f in sorted(os.listdir(gen_dir)):
            if f.startswith("code_") and f.endswith(".c"):
                src = open(os.path.join(gen_dir, f)).read()
                for m in pat.finditer(src):
                    fn = src.rfind("\nvoid f_", 0, m.start())
                    lits.setdefault(int(m.group(1), 16), set()).add(src[fn + 8:fn + 16])
    for t in TABLES:
        ok = agreement(pr, t) if t.psp is not None else None
        near = sorted(v for v in lits if t.dc - 16 <= v < t.dc)
        inside = sorted(v for v in lits if t.dc <= v < t.end)
        print(f"{t.dc:08X} x{t.size:<3} {t.kind:8} -> {t.ext:08X}  PSP {t.psp or 0:06X} {ok}/38  "
              f"literals {' '.join(f'{v:08X}' for v in inside)}"
              + (f"  BEFORE: {' '.join(f'{v:08X}' for v in near)}" if near else ""))


if __name__ == "__main__":
    check(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
