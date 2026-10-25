"""World Tour with the PSP characters: their banners on its screens.

World Tour's screens draw from bank 0x1D, WORLD.PAC (sections 4 parts, 5
tiles, 6 palettes; the PSP's world_u.pac the same), loaded into buffers
f_8C01C508 / f_8C01C5E8 allocate (0x4000, 0x30000, 0x400) and point the bank
at. A character's banner is three parts, from 8C0AB7D0[character] (u16,
moved: chartables.py) on, drawn one after another (f_8C0366D0); on the PSP
0x2C89C0, 42 entries. The new characters' three parts are added after the
Dreamcast's, and their tiles need a bigger tiles buffer: the allocations'
size is one of the code's constants the add-on sets (chartables.IMMEDIATES).
"""
import struct

import chartables
import pac
import sprites

NEW = (0x26, 0x27, 0x28, 0x29)
BANNERS = 0x8C0AB7D0      # u16 per character: the first of its banner's 3 parts
PSP_BANNERS = 0x2C89C0
PARTS_PER = 3
PARTS_ROOM, TILES_ROOM = 0x4000, 0x30000
TILES_SIZE = (0x8C01C520, 0x8C01C600)   # the allocations' size (chartables.IMMEDIATES)


def build(dc_world, psp_world, boot):
    """(WORLD.PAC, [(address, bytes)] patches)"""
    d, p = pac.sections(dc_world), pac.sections(psp_world)
    db = sprites.Bank(d[4], d[5], True)
    pb = sprites.Bank(p[4], p[5], False)
    tables = {t.dc: t for t in chartables.layout()}
    patches = []
    for c in NEW:
        base = struct.unpack_from("<H", boot, PSP_BANNERS + 0x74 + 2 * c)[0]
        first = [db.add_part(pb, base + k) for k in range(PARTS_PER)]
        assert first == list(range(first[0], first[0] + PARTS_PER)), first
        patches.append((tables[BANNERS].ext + 2 * c, struct.pack("<H", first[0])))
    d[4], d[5] = db.encode()
    assert len(d[4]) <= PARTS_ROOM, len(d[4])
    room = (len(d[5]) + 0xFFF) & ~0xFFF
    if room > TILES_ROOM:
        for a in TILES_SIZE:
            patches.append((chartables.immediate(a), struct.pack("<I", room)))
    return pac.build(pac.order(dc_world), d), patches


if __name__ == "__main__":
    import sys
    out, patches = build(open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read(), open(sys.argv[3], "rb").read())
    print(len(out), [(hex(a), v.hex()) for a, v in patches])
