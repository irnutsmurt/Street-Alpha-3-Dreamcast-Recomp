"""The arcade select screen ("SELECT PLAYER") with the PSP characters: the
PSP's layout (Yun and Ingrid at the ends of the fifth row, Eagle and Maki in
the last row where the Dreamcast has two random squares) and their faces,
names and big portraits from the PSP's select_u.pac.

Writes the add-on's SELECT.PAC (the Dreamcast's, the PSP characters' sprites
and palettes added after its own) and returns the patches to the game's data
that go with it: the grid, the face layout, and the new characters' entries
in the moved select tables (chartables.py). docs/dreamcast.md (Select
screens) has the formats.

    python3 tools/psp/screens.py <Dreamcast files dir> <PSP data dir> <PSP BOOT.BIN> <out dir>
"""
import os
import struct
import sys

import chartables
import pac
import sprites

NEW = (0x26, 0x27, 0x28, 0x29)

# --- the Dreamcast (1ST_READ.BIN) ---
ARCADE_GRID = 0x8C0BE3CC      # 7 x 7 cells: a character each
FACES = 0x8C0BF610            # face layout: 6-byte records (x, y, bank B part)
FRAMES = 0x8C0BF712           # the cells' frames (bank B part 0x27)
FRAME_PART = 0x27
PORTRAITS, NAMES, BANNERS, BANNER_WIDTHS = 0x8C0C03B4, 0x8C0C1194, 0x8C0C142C, 0x8C0C1478
STRIPS = 0x8C0C11F4           # the name strip down the side: u16 spacing | 0x8000, u16 part
PORTRAIT_PALETTES = 0x4C0     # + character x 6 + colour: the sprite beside the big portrait (slot 32 + 2 x side)
PALETTES_PER = 6
VS_PALETTES = 0x148           # + character x 24 + colour x 4: the VS portraits' (f_8C069D34), on both
VS_PALETTES_PER = 24
# --- the PSP (BOOT.BIN, select_u.pac) ---
PSP_PORTRAITS = 0x2256C0      # animation per character (42)
PSP_PORTRAIT_PALETTES = 0x538  # + character x 6 + colour: after the 5 x 24 VS palettes the PSP added before them
TWO_PALETTES = 0x29           # Ingrid: two a colour (PSP 0x99174: + 2 x colour, 2 palettes; psp_load.c)
PSP_FACES = {0x26: 0x33, 0x27: 0x34, 0x28: 0x35, 0x29: 0x36}       # bank B parts
PSP_NAMES = {0x26: 0x150, 0x27: 0x151, 0x28: 0x152, 0x29: 0x153}   # bank 8 parts
PSP_BANNERS = {0x26: 0xEE, 0x27: 0xEF, 0x28: 0xF0, 0x29: 0xF1}     # bank 8 parts
PSP_STRIP_SPACING = 0x226914  # u16 per character (STRIPS' first half)
PSP_STRIP_PARTS = 0x2C8ABC    # u8 per character: bank 8 part - 0xF4
MAP_STRIPS = 0x8C0C1AC8       # the world map's name strip: bank 8 part (f_8C06DC3C; its spacing 8C0C1B14)
PSP_MAP_STRIP_PARTS = 0x2C8A68  # u8 per character: bank 8 part - 0x94
# the world map's strips of the route's opponents (0.2.9): a beaten opponent's name (u16 bank 8 part per
# character), and a table of 24 parts by stage followed by the player's own name by character (entry 24 + c)
MAP_BEATEN, PSP_MAP_BEATEN = 0x8C0C1BEC, 0x227190
MAP_NAMES, PSP_MAP_PLAYER = 0x8C0C1C48, 0x227224   # the PSP's characters' entries (its stages' come before)
MAP_NAMES_STAGES = 24
CELLS = {0x28: (5, 0), 0x29: (5, 6), 0x26: (6, 2), 0x27: (6, 4)}  # the PSP's arcade grid
FACE_PARTS = 0x8C0BE45A       # a character's face, a bank B part (moved, chartables.py)
# versus mode's select screen: the grid (7 x 6, room after it for a 7th row) and its layout (the
# faces' list, then the frames' from record 41): rows at y -64 + 24 x row, columns x -88 + 24 x col
VS_GRID, VS_LAYOUT = 0x8C0C1D84, 0x8C0C0714
VS_CELLS = {0x26: 1, 0x27: 2, 0x28: 4, 0x29: 5}  # the PSP's fifth row: Eagle, Maki, (random), Yun, Ingrid
VS_ROW, VS_ENTRIES_Y, VS_ROW_Y = 5, 80, 56
NAME_COPIES = (0x8C0AB820, 0x8C0C0D26)  # tables holding the same name parts as NAMES
# the pre-fight dialogue's faces: a bank 8 part (a chain) per character, the face object's
# (type 14, id 6: 8C06A8BA) +0x44 (8C06A948); its palette is the VS portrait's (f_8C069D34).
# The PSP's numbers are the Dreamcast's less 0x3B, and four more
DIALOGUE_FACES, PSP_DIALOGUE_FACES = 0x8C0C0E04, 0x226098
# the VS screen's portraits: objects set their animation to group + 8 x
# character, the group from ANIM_GROUPS (a pointer per kind of object). Two
# groups of 8-byte records (one part each, a chain: the part records' byte 10
# says another part follows): VS_PORTRAITS bank B, VS_NAMES bank 8. Neither
# has room after its 38 records: both are copied with 42 to the extension
# RAM and ANIM_GROUPS (moved, chartables.py) points there.
ANIM_GROUPS, ANIM_GROUP_COUNT = 0x8C0BED8C, 30
VS_PORTRAITS, VS_NAMES = 0x8C0BEB3C, 0x8C0BEC64
PSP_VS_PORTRAITS, PSP_VS_NAMES = 0x2237AC, 0x2238FC  # 42 records each
# the fight's HUD faces: the select grid's faces (bank B) through another
# table of groups (15 kinds), the faces' group its 5th
HUD_ANIM_GROUPS, HUD_ANIM_GROUP_COUNT = 0x8C0C0C60, 15
HUD_FACES = 0x8C0C0AA6


def cell_xy(row, col):
    return col * 24 - 88, row * 24 - 88


def layout(main, a):
    """the records of a layout list: (address, x, y, part)"""
    out = []
    while True:
        x, y, v = struct.unpack_from("<hhH", main, a - chartables.MAIN)
        out.append((a, x, y, v))
        a += 6
        if v & 0x8000:
            return out


def anim(boot, a):
    """an animation (8-byte records) from the PSP: the records, to the one that jumps back"""
    recs = []
    while True:
        r = boot[a + 0x74:a + 0x74 + 8]
        recs.append(r)
        a += 8
        if r[1] & 0x80 or len(recs) > 64:
            return recs


def add_chain(dst, src, k):
    """part k of src and the parts chained after it (record byte 10 set) added
    to dst one after another: the first's new number"""
    first = dst.add_part(src, k)
    while src.records[k][0][10]:
        k += 1
        dst.add_part(src, k)
    return first


def anim_record(part, duration=8):
    """a one-record animation that holds the part (as the groups' records)"""
    return struct.pack("<BBBBHh", duration, 0x40, 0, 0, part, 0)


def build(dc_select, psp_select, main, boot):
    """(SELECT.PAC, [(address, bytes)] patches, what the runtime needs: the
    new big portraits' palette base, [(pose, extra list)] for
    hle_layout_draw, the shift of the character number that makes
    f_8C069D34's index the new VS palettes')"""
    tables = {t.dc: t for t in chartables.layout()}
    d, p = pac.sections(dc_select), pac.sections(psp_select)
    patches = []
    # bank B: the faces (32x32)
    bb = sprites.Bank(d[0x0100000B], d[0x0200000B], True, group=4)
    pb = sprites.Bank(p[0x0100000B], p[0x0200000B], False, group=4)
    faces = {c: bb.add_part(pb, PSP_FACES[c]) for c in NEW}
    portraits = {}
    for c in NEW:
        k = struct.unpack_from("<H", boot, PSP_VS_PORTRAITS + 0x74 + 8 * c + 4)[0]
        portraits[c] = add_chain(bb, pb, k)
    d[0x0100000B], d[0x0200000B] = bb.encode()
    # bank 8: names, banners, big portraits; their palettes
    b8 = sprites.Bank(d[0x01000008], d[0x02000008], True)
    p8 = sprites.Bank(p[0x01000008], p[0x02000008], False)
    pal = d[0x04000008]
    base = len(pal) // 32
    psp_pal = p[0x04000008]
    new_pal = bytearray()
    parts8 = {}

    def part8(k):
        if k not in parts8:
            parts8[k] = b8.add_part(p8, k)
        return parts8[k]
    data = chartables.EXT_SELECT
    for i, c in enumerate(NEW):
        t = tables[NAMES]
        patches.append((t.ext + 2 * c, struct.pack("<H", part8(PSP_NAMES[c]))))
        t = tables[BANNERS]
        bp = part8(PSP_BANNERS[c])
        patches.append((t.ext + 2 * c, struct.pack("<H", bp)))
        w = p8.records[PSP_BANNERS[c]][0][4]
        spacing = struct.unpack_from("<H", boot, PSP_STRIP_SPACING + 0x74 + 2 * c)[0]
        strip = part8(boot[PSP_STRIP_PARTS + 0x74 + c] + 0xF4)
        patches.append((tables[STRIPS].ext + 4 * c, struct.pack("<HH", spacing, strip)))
        patches.append((tables[MAP_STRIPS].ext + 2 * c, struct.pack("<H", part8(boot[PSP_MAP_STRIP_PARTS + 0x74 + c] + 0x94))))
        k = struct.unpack_from("<H", boot, PSP_MAP_BEATEN + 0x74 + 2 * c)[0]
        patches.append((tables[MAP_BEATEN].ext + 2 * c, struct.pack("<H", part8(k))))
        k = struct.unpack_from("<H", boot, PSP_MAP_PLAYER + 0x74 + 2 * c)[0]
        patches.append((tables[MAP_NAMES].ext + 2 * (MAP_NAMES_STAGES + c), struct.pack("<H", part8(k))))
        patches.append((tables[BANNER_WIDTHS].ext + 2 * c, struct.pack("<H", 16 * w + 32)))
        # the big portrait: the PSP's animation, its parts renumbered
        recs = bytearray()
        for r in anim(boot, struct.unpack_from("<I", boot, PSP_PORTRAITS + 0x74 + 4 * c)[0]):
            r = bytearray(r)
            struct.pack_into("<H", r, 4, part8(struct.unpack_from("<H", r, 4)[0]))
            recs += r
        patches.append((data, bytes(recs)))
        patches.append((tables[PORTRAITS].ext + 4 * c, struct.pack("<I", data)))
        data += (len(recs) + 3) & ~3
        lo = (PSP_PORTRAIT_PALETTES + c * PALETTES_PER) * 32
        n = PALETTES_PER * (2 if c == TWO_PALETTES else 1)
        new_pal += pac.swap_rb(psp_pal[lo:lo + n * 32])
    # the VS screen: the groups with the new characters' records
    vs_names = {c: add_chain(b8, p8, struct.unpack_from("<H", boot, PSP_VS_NAMES + 0x74 + 8 * c + 4)[0])
                for c in NEW}
    groups = {}
    for group, new in ((VS_PORTRAITS, portraits), (VS_NAMES, vs_names), (HUD_FACES, faces)):
        recs = bytearray(main[group - chartables.MAIN:group - chartables.MAIN + 8 * 38])
        for c in NEW:
            recs += anim_record(new[c], recs[0])
        patches.append((data, bytes(recs)))
        groups[group] = data
        data += len(recs)
    for table, count in ((ANIM_GROUPS, ANIM_GROUP_COUNT), (HUD_ANIM_GROUPS, HUD_ANIM_GROUP_COUNT)):
        t = tables[table]
        for k in range(count):  # the moved table: the extended groups' copies, the others as they were
            g = struct.unpack_from("<I", main, table - chartables.MAIN + 4 * k)[0]
            patches.append((t.ext + 4 * k, struct.pack("<I", groups.get(g, g))))
    for c in NEW:  # the names' other tables
        for a in NAME_COPIES:
            patches.append((tables[a].ext + 2 * c, struct.pack("<H", part8(PSP_NAMES[c]))))
    for c in NEW:  # the dialogue faces
        k = add_chain(b8, p8, struct.unpack_from("<H", boot, PSP_DIALOGUE_FACES + 0x74 + 2 * c)[0])
        patches.append((tables[DIALOGUE_FACES].ext + 2 * c, struct.pack("<H", k)))
    d[0x01000008], d[0x02000008] = b8.encode()
    # the VS portraits' palettes: after the big portraits', where the
    # Dreamcast's index (0x148 + 24 x character) can reach them with a
    # bigger character number (psp_load.c, hle_vs_palette)
    vs = base + len(new_pal) // 32
    vs += -(vs - VS_PALETTES) % VS_PALETTES_PER
    new_pal += bytes(32 * (vs - base) - len(new_pal))
    lo = (VS_PALETTES + NEW[0] * VS_PALETTES_PER) * 32
    new_pal += pac.swap_rb(psp_pal[lo:lo + len(NEW) * VS_PALETTES_PER * 32])
    vs_shift = (vs - VS_PALETTES) // VS_PALETTES_PER - NEW[0]
    d[0x04000008] = pal + bytes(new_pal)
    # the arcade grid: the PSP's cells for the new characters
    for c, (r, col) in CELLS.items():
        patches.append((ARCADE_GRID + 7 * r + col, bytes([c])))
    # the face layout: the random squares in the last row become faces; the
    # fifth row's two new cells aren't in the Dreamcast's lists, which other
    # objects' lists follow: they're drawn from extra lists after the face
    # and frame lists (psp_load.c, hle_layout_draw), a face and a frame each
    recs = layout(main, FACES)
    by_cell = {((y + 88) // 24, (x + 88) // 24): a for a, x, y, v in recs}
    extra_faces, extra_frames = bytearray(), bytearray()
    for c, (r, col) in CELLS.items():
        x, y = cell_xy(r, col)
        if (r, col) in by_cell:
            patches.append((by_cell[(r, col)] + 4, struct.pack("<H", faces[c])))
        else:
            extra_faces += struct.pack("<hhH", x, y, faces[c])
            extra_frames += struct.pack("<hhH", x, y, FRAME_PART)
    for lst in (extra_faces, extra_frames):  # the last record ends the list
        struct.pack_into("<H", lst, len(lst) - 2, struct.unpack_from("<H", lst, len(lst) - 2)[0] | 0x8000)
    extras = []
    for pose, lst in ((0, extra_faces), (len(recs), extra_frames)):
        patches.append((data, bytes(lst)))
        extras.append((FACES, pose, data))
        data += (len(lst) + 3) & ~3
    # versus mode's select screen. The PSP's has 7 rows: the new characters in the fifth, where
    # the Dreamcast has the World Tour entries (f_8C06EFAC fills them in: cells 35 + column, records
    # at y 56), the entries in a seventh. Here too: the entries go down a row (their cells' 35 is
    # 42, their faces' and frames' records' y 80), the cursor's last row (f_8C070648) and the
    # entries' row (f_8C0703CC) are 6, and the SPEED and STAGE lines (f_8C06F7A4) go down 24 to
    # make room (chartables.IMMEDIATES). The two random cells stay at the ends of row 4.
    for c, col in VS_CELLS.items():
        patches.append((VS_GRID + 7 * VS_ROW + col, bytes([c])))
        patches.append((tables[FACE_PARTS].ext + 2 * c, struct.pack("<H", faces[c])))
    vfaces = layout(main, VS_LAYOUT)
    vframes = layout(main, VS_LAYOUT + 6 * len(vfaces))
    for a, x, y, v in vfaces + vframes:
        if y == VS_ROW_Y:
            patches.append((a + 2, struct.pack("<h", VS_ENTRIES_Y)))
    vs_faces, vs_frames = bytearray(), bytearray()
    for c, col in VS_CELLS.items():
        x = col * 24 - 88
        vs_faces += struct.pack("<hhH", x, VS_ROW_Y, faces[c])
        vs_frames += struct.pack("<hhH", x, VS_ROW_Y, FRAME_PART)
    for pose, lst in ((0, vs_faces), (len(vfaces), vs_frames)):
        struct.pack_into("<H", lst, len(lst) - 2, struct.unpack_from("<H", lst, len(lst) - 2)[0] | 0x8000)
        patches.append((data, bytes(lst)))
        extras.append((VS_LAYOUT, pose, data))
        data += (len(lst) + 3) & ~3
    for a, v in ((0x8C0706E4, 6), (0x8C0703E6, 6), (0x8C06F0BE, 42), (0x8C06F138, 42), (0x8C06F162, 42),
                 (0x8C06F85A, 0xAE + 24), (0x8C06F868, 0xAE + 24), (0x8C06F892, 0xBE + 24), (0x8C06F8A4, 0xBE + 24)):
        patches.append((chartables.immediate(a), struct.pack("<I", v)))
    assert data <= chartables.EXT_END, "the select screens' records outgrew their room"
    # the PSP's VS groups (42 records, its bank B / 8 numbering) have these as counterparts: the ending's
    # portraits (types 73 and 74, build_char.py) read them through a table of the PSP's (0x223A4C)
    psp_groups = [(PSP_VS_PORTRAITS, PSP_VS_PORTRAITS + 8 * 42, groups[VS_PORTRAITS]),
                  (PSP_VS_NAMES, PSP_VS_NAMES + 8 * 42, groups[VS_NAMES])]
    return pac.build(pac.order(dc_select), d), patches, dict(portrait_palettes=base, layout_extras=extras,
                                                             vs_palette_shift=vs_shift, psp_groups=psp_groups)


def main():
    dc_dir, psp_data, boot_path, out_dir = sys.argv[1:5]
    main_prog = open(os.path.join(dc_dir, "1ST_READ.BIN"), "rb").read()
    out, patches, info = build(open(os.path.join(dc_dir, "SELECT.PAC"), "rb").read(),
                               open(os.path.join(psp_data, "select_u.pac"), "rb").read(),
                               main_prog, open(boot_path, "rb").read())
    open(os.path.join(out_dir, "SELECT.PAC"), "wb").write(out)
    print(f"SELECT.PAC: {len(out)} bytes, {len(patches)} patches, {info}")


if __name__ == "__main__":
    main()
