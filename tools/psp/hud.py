"""The fight's HUD with the PSP characters: their faces and names
(PITU.PAC, the PSP's pit_u.pac: the same sections).

f_8C06443C sets the HUD's parts each frame: player 1's face bank 0x10 part
0x17D + character, player 2's 0x1A6 + character; the names bank 9 parts
0xF6 / 0x121 + 8C0BE266[character] (moved, chartables.py). For 0x26 and up
those run into other parts. The PSP's code does the same with its own bases
(faces 0x17D / 0x1AA + 0x222D76[character], names 0xF6 / 0x125 +
0x222D4C[character]). The PSP's parts for the new characters are added after
the Dreamcast's, and the runtime (psp_load.c, hle_hud) puts them in.

Bank 9's tiles are the 16x16 cells of the three textures (0x10000100-102,
256x256, ARGB1555; tile n is texture n / 256, cell n % 256, row by row): the
names' cells are copied from the PSP's textures (linear, ABGR) into cells no
part uses. The two sections that grow past their fixed places (bank 9's
parts, bank 0x10's tiles) go to the extension RAM: the per-bank destination
tables the loader reads (parts 8C09AE80, tiles 8C09AF10) are patched.
"""
import struct

import pac
import sprites

NEW = (0x26, 0x27, 0x28, 0x29)
DC_FACES, DC_NAMES = (0x17D, 0x1A6), (0xF6, 0x121)
PSP_FACES, PSP_NAMES = (0x17D, 0x1AA), (0xF6, 0x125)
PSP_FACE_INDEX, PSP_NAME_INDEX = 0x222D76, 0x222D4C
DC_NAME_INDEX = 0x8C0BE266   # u8 per character: the name's index (the original, before chartables moves it)
TEXTURES = (0x10000100, 0x10000101, 0x10000102)
PARTS_DEST, TILES_DEST = 0x8C09AE80, 0x8C09AF10   # per bank: where the loader puts a bank's parts, tiles
EXT_BANK9_PARTS, EXT_BANK10_TILES = 0x98900000, 0x98910000


def parts_records(sec):
    """a parts section's records: [(12-byte record, [(tile, attr) | None])]"""
    n = struct.unpack_from("<I", sec, 0)[0] // 12
    out = []
    for k in range(n):
        o, w, h = struct.unpack_from("<IBB", sec, 12 * k)
        p, tl = o, []
        for _ in range(w * h):
            if sec[p] == 0xFF:
                tl.append(None)
                p += 1
                continue
            tl.append((sec[p] << 8 | sec[p + 1], sec[p + 2]))
            p += 3
        out.append((sec[12 * k:12 * k + 12], tl))
    return out


def parts_section(old, records):
    """old's bytes kept, records after old's appended (sprites.Bank.encode's way)"""
    n0 = struct.unpack_from("<I", old, 0)[0] // 12
    n = len(records)
    head = bytearray(old[:12 * n0])
    for k in range(n0):
        struct.pack_into("<I", head, 12 * k, struct.unpack_from("<I", head, 12 * k)[0] + 12 * (n - n0))
    body = bytearray(old[12 * n0:])
    for rec, tl in records[n0:]:
        r = bytearray(rec)
        struct.pack_into("<I", r, 0, 12 * n + len(body))
        head += r
        for e in tl:
            body += b"\xff" if e is None else bytes([e[0] >> 8, e[0] & 0xFF, e[1]])
    return bytes(head + body)


def twid(x, y):
    r = 0
    for b in range(8):
        r |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1)
    return r


def copy_cell(dst, dst_cell, src, src_cell):
    """a 16x16 cell from a PSP texture section (linear, ABGR1555) to a
    Dreamcast one (twiddled, ARGB1555); both 256x256 after a 32-byte header"""
    sx, sy = 16 * (src_cell % 16), 16 * (src_cell // 16)
    dx, dy = 16 * (dst_cell % 16), 16 * (dst_cell // 16)
    for y in range(16):
        for x in range(16):
            v = struct.unpack_from("<H", src, 32 + 2 * ((sy + y) * 256 + sx + x))[0]
            v = (v & 0x83E0) | ((v & 0x1F) << 10) | ((v >> 10) & 0x1F)
            struct.pack_into("<H", dst, 32 + 2 * twid(dx + x, dy + y), v)


def build(dc_pitu, psp_pit, main, boot):
    """(PITU.PAC, [(address, bytes)] patches, {character: (face 1, face 2, name 1, name 2)})"""
    d, p = pac.sections(dc_pitu), pac.sections(psp_pit)
    psp_byte = lambda a: boot[a + 0x74]
    dc_byte = lambda a: main[a - 0x8C010000]
    # faces: bank 0x10
    db = sprites.Bank(d[0x1000010], d[0x2000010], True)
    pb = sprites.Bank(p[0x1000010], p[0x2000010], False)
    faces = {c: tuple(db.add_part(pb, base + psp_byte(PSP_FACE_INDEX + c)) for base in PSP_FACES) for c in NEW}
    d[0x1000010], d[0x2000010] = db.encode()
    # names: bank 9, cells of the textures
    recs = parts_records(d[0x1000009])
    precs = parts_records(p[0x1000009])
    used = {e[0] for _, tl in recs for e in tl if e}
    free = [n for n in range(256 * len(TEXTURES)) if n not in used]
    texs = {t: bytearray(d[t]) for t in TEXTURES}
    # the Dreamcast's names are two cells high, the text in the lower row (the
    # PSP's one row): a new name gets an empty row above, and the placement
    # bytes of a Dreamcast name of its width on its side
    dc_index = [dc_byte(DC_NAME_INDEX + k) for k in range(0x26)]
    templates = [{} for _ in DC_NAMES]
    for side, base in enumerate(DC_NAMES):
        for k in dc_index:
            rec = recs[base + k][0]
            templates[side].setdefault(rec[4], rec)
    copied, names = {}, {}
    for c in NEW:
        mine = []
        for side, base in enumerate(PSP_NAMES):
            rec, tl = precs[base + psp_byte(PSP_NAME_INDEX + c)]
            w = rec[4]
            if rec[5] != 1 or w not in templates[side]:
                raise ValueError(f"character {c:02X}'s name: {rec.hex()} has no Dreamcast counterpart")
            new_tl = [None] * w
            for e in tl:
                if e is None:
                    new_tl.append(None)
                    continue
                if e[0] not in copied:
                    n = free.pop(0)
                    copy_cell(texs[TEXTURES[n // 256]], n % 256, p[TEXTURES[e[0] // 256]], e[0] % 256)
                    copied[e[0]] = n
                new_tl.append((copied[e[0]], e[1]))
            r = bytearray(templates[side][w])
            r[5] = 2
            recs.append((bytes(r), new_tl))
            mine.append(len(recs) - 1)
        names[c] = tuple(mine)
    d[0x1000009] = parts_section(d[0x1000009], recs)
    for t in TEXTURES:
        d[t] = bytes(texs[t])
    patches = [(PARTS_DEST + 4 * 9, struct.pack("<I", EXT_BANK9_PARTS)),
               (TILES_DEST + 4 * 0x10, struct.pack("<I", EXT_BANK10_TILES))]
    assert len(d[0x1000009]) <= EXT_BANK10_TILES - EXT_BANK9_PARTS
    parts = {c: faces[c] + names[c] for c in NEW}
    return pac.build(pac.order(dc_pitu), d), patches, parts


if __name__ == "__main__":
    import sys
    out, patches, parts = build(open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read(),
                                open(sys.argv[3], "rb").read(), open(sys.argv[4], "rb").read())
    print(len(out), {f"{c:02X}": [hex(x) for x in v] for c, v in parts.items()})
