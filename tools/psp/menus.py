"""The mode menu's archive (MSEL.PAC) with the PSP characters' faces.

MSEL.PAC's sprites (sections 7 parts, 8 tiles, 9 palettes; bank 0x12 when
loaded) hold the menus' small faces: the score ranking's lists draw a
character's face from a byte table by character (8C0D2B01; the other menu
modules have copies at 8C0D3045 and 8C0D3D1D), 37 entries, mirrored by a u16
table (8C0D2AB2, copies 8C0D2FF6 and 8C0D3CCA; its low byte to the face
object's +0xB). A new character in a ranking read past them (garbage
sprites, faces upside down). The PSP's archive has the new characters' faces
after the others (its parts 0x80 + its table 0x2C8C60, 42 entries: Eagle
0xB3, Maki 0xB4, Yun 0xB5, Ingrid 0xB6; their palettes the Dreamcast's),
mirrored by its byte table 0x2C8C8A. They're added to the Dreamcast's bank
and their numbers go in the moved tables (chartables.py).

    python3 tools/psp/menus.py <Dreamcast files dir> <PSP data dir> <PSP BOOT.BIN> <out dir>
"""
import os
import sys

import chartables
import pac
import sprites

NEW = (0x26, 0x27, 0x28, 0x29)
FACE_TABLES = (0x8C0D2B01, 0x8C0D3045, 0x8C0D3D1D)  # a bank part per character, one per menu module
PSP_FACES, PSP_FACE_BASE = 0x2C8C60, 0x80           # the PSP's: part PSP_FACE_BASE + PSP_FACES[character]
MIRROR_TABLES = (0x8C0D2AB2, 0x8C0D2FF6, 0x8C0D3CCA)  # u16 per character: the face mirrored
PSP_MIRROR = 0x2C8C8A                               # the PSP's: a byte per character
PARTS, TILES = 7, 8                                 # the archive's sections


def build(dc_msel, psp_msel, boot):
    """(MSEL.PAC, [(address, bytes)] patches: the new characters' entries in the face tables)"""
    tables = {t.dc: t for t in chartables.layout()}
    d, p = pac.sections(dc_msel), pac.sections(psp_msel)
    db = sprites.Bank(d[PARTS], d[TILES], True)
    pb = sprites.Bank(p[PARTS], p[TILES], False)
    patches = []
    for c in NEW:
        k = db.add_part(pb, PSP_FACE_BASE + boot[PSP_FACES + 0x74 + c])
        assert k < 0x100
        patches += [(tables[a].ext + c, bytes([k])) for a in FACE_TABLES]
        patches += [(tables[a].ext + 2 * c, bytes([boot[PSP_MIRROR + 0x74 + c], 0])) for a in MIRROR_TABLES]
    d[PARTS], d[TILES] = db.encode()
    return pac.build(pac.order(dc_msel), d), patches


def main():
    dc_dir, psp_data, boot_path, out_dir = sys.argv[1:5]
    msel, patches = build(open(os.path.join(dc_dir, "MSEL.PAC"), "rb").read(),
                          open(os.path.join(psp_data, "msel_u.pac"), "rb").read(), open(boot_path, "rb").read())
    open(os.path.join(out_dir, "MSEL.PAC"), "wb").write(msel)
    print(f"MSEL.PAC: {len(msel)} bytes, {len(patches)} patches")


if __name__ == "__main__":
    main()
