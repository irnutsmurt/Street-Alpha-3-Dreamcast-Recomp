"""The PSP characters' arcade endings in the Dreamcast's form: ENDnn.BIN (the
overlay) and ENDnn.PAC (its textures and sprites).

The Dreamcast: the ending task (f_8C025ECA) has f_8C01BA82 load endNN.bin to
8C800000 and the archive names[8C09AC12[character]], then runs the overlay.
Every ending's overlay is the same code (to 0xBD08), then the character's
data, a directory of it (13 words; its address is the code's literal at
0x328), variables (zero in the file) and a few library routines (memcpy and
the like). The code's only references past 0xBD08 are the directory, the
variables and the routines. The setup copies directory words 0-10 into the
ending's state and gives word 11 to f_8C0218B6. The directory:

  [0]  the script: the PSP's section 0x20000
  [1]  the PSP directory's [0]
  [2]  pointers to section 0x20001's 10-byte records, by 0x20003's u16
       indices (the last index, 0xFFFF, dropped; 0xFFFF before it: 0)
  [3]  section 0x20005
  [4]  pointers to 0x20002's 8-byte records, by 0x20004's indices
  [5]  the palettes: section 0x4000019 (ARGB here, ABGR on the PSP)
  [6]  the PSP directory's [1]
  [7]  the PSP directory's [2]: pointers to palettes (blocks the PSP's
       endings share; each Dreamcast ending has its own copy)
  [8]  the PSP directory's [3]
  [9]  the PSP directory's [4] (0 for most)
  [10] pointers to the text's records (section 0x10003, a record per 128
       bytes: the characters to type, then one or two lines, each an x
       position and its text to a NUL; packed here)
  [11] the PSP directory's [5]: (texture, palette) byte pairs to 0xFF
  [12] the closing screen's logo (Capcom's): its part number in sprite
       bank 0x19 (the screen's object, type 76, draws from it by mode 3:
       8C0179F2; read as a u16); here: the add-on's mark (below)
The PSP's directories are in BOOT.BIN (a table at 0x2B34B0 by character),
next to the data they point to; its relocations mark the pointers. Records
in 0x20001 and 0x20002 name parts of sprite bank 0x19 by number: the
numbers follow each platform's bank, so the PSP's bank comes too.

The PSP's ending draws people (actors, ending object type 75 and others)
with the sprites of the fighters still loaded: an actor's 32-byte record
ends with a character number (object +0x39 here, +0x49 on the PSP), and
each animation set from 0x20001 (0x1D4C40, the Dreamcast's 8C806DF2 &c.
without it) takes the bank of the fighter with that character, or for one
neither fighter is (Guile in Eagle's ending, whose sprites are at the end of
Eagle's file) the fighter that isn't M.Bison's; 0x20001's pose numbers are
those fighters' own. The Dreamcast's endings have copies of those sprites
in bank 0x19 instead. The runtime does as the PSP (hle_ending_anim on
f_8C024AD4), for an ending whose directory word 12 points at the mark:
"PSPE", then 0x20001's records' start and end, then the logo's part.

The closing screen (THANK YOU FOR PLAYING, PRESENTED BY, the logo, the
url): the Dreamcast's last step (8C800C30) makes it, object type 76, in
every ending. The PSP's handler for it (0x1D548C) takes the logo's part from
its ending state's +0x138, which its setup zeroes and nothing else writes
(the PSP makes no type 76 object: dead code there); the Dreamcast keeps
another struct at the state's +0x138 (8C1A330C). The PSP's banks have the
logo as their own part (end26 0x6A, end27 0, end28 5; end29 none): the add-on
finds it (the template's logo, pixel for pixel), or copies it into the bank
from a Dreamcast ending's, and the recompiled handler reads it from the mark
(build_char.py REPLACE, psp_load.c psp_ending_logo).

The PSP's object handlers run for these endings in place of the
Dreamcast's (build_char.py ENDING_HANDLERS, psp_load.c hle_ending_setup).

The add-on's overlay keeps a Dreamcast ending's code, directory address,
variables and routines (the template: one with room for the data), so its
code is byte for byte the template's, which the recompiler knows; the PSP
character's data goes between.

The archive: the sections all endings share from the template's (0x0,
textures 0xC00-0xC16), the PSP's per character: 0x10, the textures from
0xC17 (PVR, the Dreamcast's headers; pixels linear and ABGR on the PSP,
twiddled and ARGB here), sprite bank 0x19 (tiles twiddled). Texture 0x900
(the text in Japanese) blank.

    python3 tools/psp/ending.py <BOOT.BIN> <PSP data dir> <Dreamcast files dir> [check | <out dir> [character ...]]
"""
import os
import struct
import sys

import mipsgen
import pac
import sprites

NEW = (0x26, 0x27, 0x28, 0x29)
BASE = 0x8C800000
CODE_END = 0xBD08          # the code (the same in every ending) ends here
DIR_LITERAL = 0x328        # the code's literal: the directory's address
DIR_WORDS = 13
CLEAN = (0x6ED8, 8)        # words END07 and END10 set in the code area: zero in a template
PSP_DIRS = 0x2B34B0        # BOOT.BIN: the ending directory by character (42)
PSP_DIR_WORDS = 6
SHARED = 0x2C5E60          # the PSP's palettes the endings share: from here
DC_FROM_PSP = (1, 6, 7, 8, 9, 11)   # directory words from the PSP directory's 0-5
LINE = 128                 # the PSP's text: a line per 128 bytes
MARK = b"PSPE"             # directory word 12 -> MARK, 0x20001's records' start and end (hle_ending_anim), the logo's part
LOGO_FROM = "END00"        # the Dreamcast ending whose closing logo (directory word 12's part) the add-on looks for / copies
TEXTURES_SHARED = range(0x10000C00, 0x10000C17)
TEXTURE_JP = 0x10000900


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def twid(x, y):
    r = 0
    for k in range(8):
        r |= ((y >> k) & 1) << (2 * k) | ((x >> k) & 1) << (2 * k + 1)
    return r


TWID = [twid(i & 255, i >> 8) for i in range(65536)]


def texture(sec):
    """a PSP texture section (PVR header, then pixels linear; 16-bit ones ABGR) as
    the Dreamcast's (twiddled, ARGB): 256x256, ARGB1555 or 4-bit palettized"""
    fmt, size = sec[16 + 9], struct.unpack_from("<HH", sec, 16 + 12)
    assert size == (256, 256) and fmt in (1, 5), (fmt, size)
    out = bytearray(sec)
    if fmt == 1:
        px = struct.unpack_from("<65536H", sec, 32)
        for i, v in enumerate(px):
            struct.pack_into("<H", out, 32 + 2 * TWID[i], (v & 0x83E0) | ((v & 0x1F) << 10) | ((v >> 10) & 0x1F))
    else:
        out[32:] = bytes(len(sec) - 32)
        for i in range(65536):
            j = TWID[i]
            out[32 + j // 2] |= ((sec[32 + i // 2] >> (4 * (i & 1))) & 15) << (4 * (j & 1))
    return bytes(out)


class Psp:
    """the PSP's ending data for a character: its directory and the blocks it
    reaches in BOOT.BIN (a block runs to the next address anything points at)"""
    def __init__(self, boot, c):
        self.boot = boot
        self.dir = boot.u32(PSP_DIRS + 4 * c)
        self.entries = [boot.u32(self.dir + 4 * k) for k in range(PSP_DIR_WORDS)]
        targets = {boot.u32(p) for p in boot.data_ptrs} | {boot.u32(PSP_DIRS + 4 * k) for k in range(42)}
        self.targets = sorted(t for t in targets if mipsgen.DATA <= t)
        self.pieces = {}            # start -> end
        self.palettes = set()       # pieces that are colours (the PSP directory's [2] points at them)
        todo = [e for e in self.entries if e]
        while todo:
            a = todo.pop()
            if a in self.pieces:
                continue
            end = self.next_target(a)
            self.pieces[a] = end
            for p in range(a & ~3, end, 4):
                if p in boot.data_ptrs and boot.u32(p):
                    todo.append(boot.u32(p))
        p7 = self.entries[2]
        for p in range(p7, self.pieces[p7], 4):
            if p in boot.data_ptrs and boot.u32(p):
                self.palettes.add(boot.u32(p))
        self.local = sorted(a for a in self.pieces if a < SHARED)
        self.shared = sorted(a for a in self.pieces if a >= SHARED)
        assert all(a in self.palettes for a in self.shared), "a shared block other than palettes"
        assert not any(a in self.palettes for a in self.local), "a palette block of the character's own"
        self.lo, self.hi = self.local[0], max(self.pieces[a] for a in self.local)
        assert self.hi <= self.dir

    def next_target(self, a):
        import bisect
        k = bisect.bisect_right(self.targets, a)
        return self.targets[k] if k < len(self.targets) else a + 4

    def bytes(self, a, end):
        return self.boot.data[a + mipsgen.OFF:end + mipsgen.OFF]


class Overlay:
    """the overlay's data being laid out from CODE_END: chunks at addresses"""
    def __init__(self):
        self.data = bytearray()

    def put(self, b, align=4, phase=0):
        while (CODE_END + len(self.data)) % align != phase:
            self.data += b"\0"
        a = BASE + CODE_END + len(self.data)
        self.data += b
        return a


def overlay_data(boot, c, secs, logo=0x8000):
    """the character's ending data laid out from CODE_END: (bytes, directory words); logo: the closing
    screen's logo's part in bank 0x19 (0x8000: none)"""
    p = Psp(boot, c)
    o = Overlay()
    where = {}
    # the PSP's blocks: the character's as one span (they point into each
    # other's middles), then the shared palettes each
    span = bytearray(p.bytes(p.lo, p.hi))
    at = o.put(bytes(len(span)), 16, p.lo % 16)
    for a in p.shared:
        where[a] = o.put(pac.swap_rb(p.bytes(a, p.pieces[a])), 4)

    def moved(t):
        if p.lo <= t < p.hi:
            return at + (t - p.lo)
        assert t in where, f"PSP pointer {t:#x} out of the ending's data"
        return where[t]
    for q in range(p.lo & ~3, p.hi, 4):
        if q in boot.data_ptrs and boot.u32(q):
            struct.pack_into("<I", span, q - p.lo, moved(boot.u32(q)))
    o.data[at - BASE - CODE_END:at - BASE - CODE_END + len(span)] = span

    def table(recs, size, idx_sec):
        n = len(idx_sec) // 2
        idx = struct.unpack(f"<{n}H", idx_sec)
        assert idx[-1] == 0xFFFF
        return b"".join(struct.pack("<I", 0 if i == 0xFFFF else recs + size * i) for i in idx[:-1])
    d = [0] * DIR_WORDS
    r1 = o.put(secs[0x20001], 4)
    d[12] = o.put(MARK + struct.pack("<III", r1, r1 + len(secs[0x20001]), logo))
    d[2] = o.put(table(r1, 10, secs[0x20003]))
    d[3] = o.put(secs[0x20005])
    r2 = o.put(secs[0x20002], 4)
    d[4] = o.put(table(r2, 8, secs[0x20004]))
    d[0] = o.put(secs[0x20000])
    d[5] = o.put(pac.swap_rb(secs[0x4000019]))
    text = secs[0x10003]
    lines = [o.put(text_record(text[LINE * k:LINE * (k + 1)]), 1) for k in range(len(text) // LINE)]
    d[10] = o.put(b"".join(struct.pack("<I", a) for a in lines))
    for k, e in zip(DC_FROM_PSP, p.entries):
        d[k] = moved(e) if e else 0
    return bytes(o.data), d


def text_record(block):
    """a line of the PSP's text (a 128-byte block) as the Dreamcast keeps it: the number of characters
    to type, then up to two lines, each its x position and its text to a NUL"""
    end1 = block.index(b"\0", 2)
    if block[end1 + 1] == 0:  # one line (the count ends the typing before a second)
        return block[:end1 + 1] + b"\0\0"
    return block[:block.index(b"\0", end1 + 2) + 1]


def dc_directory(b):
    a = u32(b, DIR_LITERAL) - BASE
    return a, [u32(b, a + 4 * k) for k in range(DIR_WORDS)]


def templates(dc_dir):
    """the Dreamcast's ending overlays that can hold another's data: (room, name, bytes), most room first"""
    out = []
    for f in sorted(os.listdir(dc_dir)):
        if f.upper().startswith("END") and f.upper().endswith(".BIN") and len(f) == 9:
            b = open(os.path.join(dc_dir, f), "rb").read()
            if b[CLEAN[0]:CLEAN[0] + CLEAN[1]] == bytes(CLEAN[1]):
                out.append((dc_directory(b)[0] - CODE_END, f, b))
    return sorted(out, key=lambda t: -t[0])


def overlay(boot, c, secs, dc_dir, logo):
    """ENDnn.BIN for character c: (bytes, the template's name)"""
    data, d = overlay_data(boot, c, secs, logo)
    room, name, t = templates(dc_dir)[0]
    assert len(data) <= room, f"ending {c:02X}: {len(data)} bytes of data, room for {room}"
    dir_at, _ = dc_directory(t)
    out = bytearray(t[:CODE_END]) + data + bytes(room - len(data))
    out += b"".join(struct.pack("<I", w) for w in d)
    out += t[dir_at + 4 * DIR_WORDS:]
    assert len(out) == len(t)
    return bytes(out), name


def part_pixels(b, k):
    """a part's size and its tiles' pixels (linear) and attributes"""
    rec, tl = b.records[k]
    return rec[4:], [None if e is None else (bytes(b.tile_pixels(e[0])), e[1]) for e in tl]


def bank19(secs, dc_dir):
    """the character's sprite bank 0x19 the Dreamcast's way (tiles twiddled) with the closing screen's logo:
    (parts section, tiles section, the logo's first part). The logo is a pose (parts to the one whose +10 is 0)
    the same in every ending; the PSP's are their own part, or none (then the Dreamcast's is copied in)."""
    ref_bin = open(os.path.join(dc_dir, LOGO_FROM + ".BIN"), "rb").read()
    t = pac.sections(open(os.path.join(dc_dir, LOGO_FROM + ".PAC"), "rb").read())
    ref = sprites.Bank(t[0x1000019], t[0x2000019], True)
    k0 = dc_directory(ref_bin)[1][12] & 0xFFFF
    pose = [k0]
    while ref.records[pose[-1]][0][10]:
        pose.append(pose[-1] + 1)
    want = [part_pixels(ref, k) for k in pose]
    b = sprites.Bank(secs[0x1000019], pac.convert_tiles(secs[0x2000019]), True)
    for k in range(len(b.records) - len(pose) + 1):
        if all(b.records[k + i][0][4:] == ref.records[q][0][4:] for i, q in enumerate(pose)) and \
                [part_pixels(b, k + i) for i in range(len(pose))] == want:
            logo = k
            break
    else:
        logo = len(b.records)
        for q in pose:
            b.add_part(ref, q)
    parts, tiles = b.encode()
    return parts, tiles, logo


def archive(secs, template_pac, parts, tiles):
    """ENDnn.PAC: the shared sections from a Dreamcast ending's archive, the PSP's for the character (sprite
    bank 0x19: bank19's)"""
    t = pac.sections(template_pac)
    tags = [0x0, 0x10] + list(TEXTURES_SHARED)
    out = {0x0: t[0x0], 0x10: secs[0x10]}
    for k in TEXTURES_SHARED:
        out[k] = t[k]
    for k in sorted(s for s in secs if 0x10000C17 <= s <= 0x10000CFF):
        tags.append(k)
        out[k] = texture(secs[k])
    tags += [TEXTURE_JP, 0x1000019, 0x2000019]
    out[TEXTURE_JP] = t[TEXTURE_JP][:32] + bytes(len(t[TEXTURE_JP]) - 32)
    out[0x1000019] = parts
    out[0x2000019] = tiles
    return pac.build(tags, out)


def build(boot, psp_data, dc_dir, out_dir, chars=NEW):
    """writes ENDnn.BIN and ENDnn.PAC for each character: [(file name, size, template)]"""
    tpac = open(os.path.join(dc_dir, "END00.PAC"), "rb").read()
    out = []
    for c in chars:
        secs = pac.sections(open(os.path.join(psp_data, f"end{c:02x}_u.pac"), "rb").read())
        parts, tiles, logo = bank19(secs, dc_dir)
        b, tname = overlay(boot, c, secs, dc_dir, logo)
        a = archive(secs, tpac, parts, tiles)
        for name, data in ((f"END{c:02X}.BIN", b), (f"END{c:02X}.PAC", a)):
            open(os.path.join(out_dir, name), "wb").write(data)
            out.append((name, len(data), tname))
    return out


def check(boot, psp_data, dc_dir):
    """the Dreamcast's own endings rebuilt from the PSP's data, compared with
    the real ones entry by entry (contents, pointers followed)"""
    for f in sorted(os.listdir(dc_dir)):
        if not (f.startswith("END") and f.endswith(".BIN") and len(f) == 9):
            continue
        c = int(f[3:5], 16)
        real = open(os.path.join(dc_dir, f), "rb").read()
        secs = pac.sections(open(os.path.join(psp_data, f"end{c:02x}_u.pac"), "rb").read())
        data, d = overlay_data(boot, c, secs)
        gen = bytes(CODE_END) + data
        _, rd = dc_directory(real)
        p = Psp(boot, c)
        bad = []

        def same(k, a, b, n):
            x, y = gen[a - BASE:a - BASE + n], real[b - BASE:b - BASE + n]
            if x != y:
                bad.append(f"[{k}] {n} bytes differ at {b:#x}")
        for k, n in ((0, len(secs[0x20000])), (3, len(secs[0x20005])), (5, len(secs[0x4000019]))):
            same(k, d[k], rd[k], n)
        for k, size, part in ((2, 10, 4), (4, 8, 4)):
            # the records name sprite parts (bytes part, part + 1) by each platform's numbering
            n = len(secs[0x20003 if k == 2 else 0x20004]) // 2 - 1
            gi = [u32(gen, d[k] - BASE + 4 * i) for i in range(n)]
            ri = [u32(real, rd[k] - BASE + 4 * i) for i in range(n)]
            if not any(gi) or not any(ri):
                if any(gi) or any(ri):
                    bad.append(f"[{k}] no records on one side")
                continue
            g0, r0 = min(x for x in gi if x), min(x for x in ri if x)
            if [x and (x - g0) // size for x in gi] != [x and (x - r0) // size for x in ri]:
                bad.append(f"[{k}] indices differ")
            n0 = len(secs[0x20001 if k == 2 else 0x20002])
            x, y = bytearray(gen[g0 - BASE:g0 - BASE + n0]), bytearray(real[r0 - BASE:r0 - BASE + n0])
            for o in range(part, n0, size):
                x[o:o + 2] = y[o:o + 2] = b"PP"
            if x != y:
                bad.append(f"[{k}] records: {sum(i != j for i, j in zip(x, y))} bytes differ")
        lines = len(secs[0x10003]) // LINE
        gl = [u32(gen, d[10] - BASE + 4 * i) for i in range(lines)]
        rl = [u32(real, rd[10] - BASE + 4 * i) for i in range(lines)]

        def record_lines(img, a):  # the count byte, then the line or two
            e1 = img.index(b"\0", a - BASE + 2)
            first = img[a - BASE:e1]
            if img[e1 + 1] == 0 or img[a - BASE] <= len(first) - 2:
                return first
            return first, img[e1 + 1:img.index(b"\0", e1 + 2)]
        diff = sum(record_lines(gen, a) != record_lines(real, b) for a, b in zip(gl, rl))
        if diff:
            bad.append(f"text: {diff} of {lines} lines differ")
        seen = set()

        def walk(pa, ga, ra):
            if pa in seen or not pa:
                return
            seen.add(pa)
            end = p.pieces.get(pa, p.next_target(pa))
            for q in range(pa, end):
                if q % 4 == 0 and q in boot.data_ptrs and q + 4 <= end:
                    walk(boot.u32(q), u32(gen, ga - BASE + q - pa), u32(real, ra - BASE + q - pa))
            x = gen[ga - BASE:ga - BASE + end - pa]
            y = real[ra - BASE:ra - BASE + end - pa]
            ptrs = [q - pa for q in range(pa, end, 4) if q in boot.data_ptrs]
            x = bytearray(x); y = bytearray(y)
            for o in ptrs:
                x[o:o + 4] = y[o:o + 4] = b"PPPP"
            if x != y:
                bad.append(f"PSP {pa:#x} ({end - pa} bytes) -> {ra:#x}: {sum(i != j for i, j in zip(x, y))} bytes differ")
        for k, e in zip(DC_FROM_PSP, p.entries):
            if e and rd[k]:
                walk(e, d[k], rd[k])
            elif bool(e) != bool(rd[k]):
                bad.append(f"[{k}] {e:#x} vs {rd[k]:#x}")
        print(f"{f}: {'ok' if not bad else '; '.join(bad[:6])} ({len(data)} bytes of data, real {dc_directory(real)[0] - CODE_END})")


def main():
    boot = mipsgen.Boot(sys.argv[1])
    psp_data, dc_dir = sys.argv[2:4]
    if sys.argv[4] == "check":
        check(boot, psp_data, dc_dir)
        return
    chars = [int(a, 16) for a in sys.argv[5:]] or NEW
    for name, size, tname in build(boot, psp_data, dc_dir, sys.argv[4], chars):
        print(f"{name}: {size} bytes (code from {tname})")


if __name__ == "__main__":
    main()
