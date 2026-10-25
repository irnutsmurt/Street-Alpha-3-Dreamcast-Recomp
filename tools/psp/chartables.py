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
EXT_DATA = 0x98E00000    # records the PSP's new entries point at
EXT_END = 0x98F00000     # (psp_load.c keeps the archive names from here)
CHARS = 0x2A             # entries after the move: 0x00-0x29
NEW = range(0x26, 0x2A)
PSP_OFF = 0x74           # BOOT.BIN: file offset = address + 0x74
MAIN = 0x8C010000


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
    # --- files: names[] indices (the file name table 0x8C09895C); the add-on's files get their own ---
    T(0x8C09B08E, 2, None, count=40, note="win screen file (f_8C01C2A6); 0x24/0x25 are win24/win25"),
    T(0x8C09B0DE, 1, None, count=40, note="win screen: a byte per character (f_8C01C2A6)"),
    T(0x8C09B116, 2, None, count=40, note="bd file (f_8C01CA18): win portrait sprites"),
    # --- not by character: the PSP's additions the new characters use ---
    T(0x8C0B39C8, 4, 0x2178C8, "cmd", count=64, entries=66, new=range(64, 66),
      note="command definitions (f_8C04EBDC): 0x40 QCB x2 ending back + K, 0x41 QCF x2 ending forward + K"),
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
    assert data <= EXT_END
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
