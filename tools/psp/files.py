"""The PSP characters' files in the Dreamcast's form: each built from the
PSP's file for the character and a Dreamcast file of the same kind (the
sections the PSP's doesn't have, and the order).

  PLnn.PAC  the character's archive (pl26_u.pac ...): sprite parts, tiles
            (twiddled), palettes (red and blue swapped; 5 a colour choice:
            Ingrid's 8 cut to their first 5) from the PSP's; the voice bank
            (SOSB), the KO echo's (SMPB) and the voice records (0xD)
            converted (sound.py); the move list (the pause menu's ATTACK
            LIST: section 0xA, sprite bank 0x1E's parts, made of the 16x16
            cells of the texture 0x10000E00), the PSP's Japanese one made
            English (movelist.py)
  OPnn.PAC  the arcade prologue on the world map (op26_u.pac ...): bank 0x15
            (the portrait and the screen's frame), and after its palettes
            the text: the personal data (the PSP's section 0x10004, the same
            bytes) and the story, three blocks of lines (the PSP's 0x10000,
            128 bytes a block; the Dreamcast's are 69 bytes, four of them).
            The texture (0x10000A00) is the story in Japanese, drawn only in
            Japanese: left blank.
  WINnn.PAC the win screen (win26_u.pac ...): the portrait (sections 0x1
            parts, 0x2 tiles, 0x3 palettes), the win quotes (0x0: their lines
            one after another; the PSP's 0x10002 has a 128-byte block a
            quote, and a quote of one line has no empty second one on the
            Dreamcast) and the sound (0x40000000: one bank for every character,
            the same on the PSP: the template's)
  BDnn.PAC  the pre-fight dialogue's sprites (bank 0x1A, when the PSP's has
            them) and the dialogue in Japanese (a texture: left blank). The
            English dialogue is in the character file (build_char.dc_text).
docs/psp-characters.md (Files per character) has more.

    python3 tools/psp/files.py <PSP data dir> <Dreamcast files dir> <out dir> [character ...]
"""
import os
import struct
import sys

import cache
import movelist
import pac
import sound

NEW = (0x26, 0x27, 0x28, 0x29)

PALETTES = 0x880          # an OP file's palettes (68), then its text
PERSONAL = 180            # 10 lines of 18 bytes
STORY_PSP, STORY_DC, STORY_BLOCKS = 128, 69, 4


def pl_file(psp, dc, cid=None, dc_dir=None):
    """psp: the PSP's plNN_u.pac; dc: a Dreamcast character archive (PL00.PAC); with the
    character and the Dreamcast files, its move list in English (movelist.py)"""
    s, d = pac.sections(psp), pac.sections(dc)
    d[0x100000] = s[0x100000]
    d[0x200000] = pac.convert_tiles(s[0x200000])
    d[0x400000] = pac.swap_rb(palettes(s[0x400000]))
    d[0xA] = s[0xA]
    d[0x40000002], d[0x20000005] = sound.voice_bank(s[0x40000002], s[0x40008002],
                                                    d[0x40000002], d[0x20000005])[:2]
    d[0xD] = sound.voice_records(s[0xD])
    d[0x10000E00] = pac.convert_texture(s[0x10000E00], d[0x10000E00])  # the move list
    if cid in movelist.LISTS and dc_dir:
        d[0xA], d[0x10000E00] = movelist.english(cid, d, dc_dir)
    return pac.build(pac.order(dc), d)


def palettes(sec, choices=6, dc_per=5):
    """a character's palettes for its 6 colour choices as the Dreamcast keeps
    them, 5 a choice (a player has 5 slots: bases 0, 5, 10, 8C0B21AC); the
    PSP's Ingrid has 8 a choice (its players' bases are 0, 0x40, 0x48): her
    sprites use the first 5 but for one part (0x2A4, sub-palette 5)"""
    per = len(sec) // 32 // choices
    if per == dc_per:
        return sec
    return b"".join(sec[32 * per * c:32 * (per * c + dc_per)] for c in range(choices))


def op_file(psp, dc):
    """psp: the PSP's opNN_u.pac; dc: a Dreamcast prologue file (OP00.PAC)"""
    s, d = pac.sections(psp), pac.sections(dc)
    story = bytearray()
    for k in range(STORY_BLOCKS):
        block = s[0x10000][STORY_PSP * k:STORY_PSP * (k + 1)]
        if any(block[STORY_DC:]):
            raise ValueError(f"story block {k} is longer than the Dreamcast's {STORY_DC} bytes")
        story += block[:STORY_DC].ljust(STORY_DC, b"\0")
    personal = s[0x10004]
    assert len(personal) == PERSONAL, len(personal)
    d[0x1000015] = s[0x1000015]
    d[0x2000015] = pac.convert_tiles(s[0x2000015])
    d[0x4000015] = pac.swap_rb(s[0x4000015][:PALETTES]) + personal + bytes(story)
    d[0x10000A00] = blank_texture(d[0x10000A00])  # the story in Japanese
    return pac.build(pac.order(dc), d)


def pack_lines(sec, block=128):
    """text blocks of two lines (the PSP's) as the Dreamcast keeps them: the
    lines one after another, a missing second line left out"""
    out = bytearray()
    for k in range(0, len(sec), block):
        b = sec[k:k + block]
        if not any(b):
            continue
        first = b.index(0)
        second = b.index(0, first + 1)
        out += b[:first + 1] if second == first + 1 else b[:second + 1]
    return bytes(out)


def blank_texture(tex):
    """a PVR texture section with its header kept and its pixels cleared"""
    return tex[:32] + bytes(len(tex) - 32)


def win_file(psp, dc):
    """psp: the PSP's winNN_u.pac; dc: a Dreamcast win file (WIN00.PAC)"""
    s, d = pac.sections(psp), pac.sections(dc)
    d[0x1] = s[0x1]
    d[0x2] = pac.convert_tiles(s[0x2])
    d[0x3] = pac.swap_rb(s[0x3])
    d[0x0] = pack_lines(s[0x10002])
    return pac.build(pac.order(dc), d)


def bd_file(psp, dc):
    """psp: the PSP's bdNN_u.pac; dc: a Dreamcast bd file with sprites (BD00.PAC)"""
    s, d = pac.sections(psp), pac.sections(dc)
    order = pac.order(dc)
    if 0x100001A in s:
        d[0x100001A] = s[0x100001A]
        d[0x200001A] = pac.convert_tiles(s[0x200001A])
    else:
        order = [t for t in order if t not in (0x100001A, 0x200001A)]
    d[0x10000900] = blank_texture(d[0x10000900])
    return pac.build(order, d)


KINDS = {  # name -> (PSP file, Dreamcast template, converter)
    "PL": ("pl{:02x}_u.pac", "PL00.PAC", pl_file),
    "OP": ("op{:02x}_u.pac", "OP00.PAC", op_file),
    "WIN": ("win{:02x}_u.pac", "WIN00.PAC", win_file),
    "BD": ("bd{:02x}_u.pac", "BD00.PAC", bd_file),
}


def build(psp_data, dc_dir, out_dir, chars=NEW, kinds=KINDS):
    """writes each kind's file for each character: [(file name, size)]"""
    out = []
    for kind, (psp_name, template, conv) in kinds.items():
        dc = open(os.path.join(dc_dir, template), "rb").read()
        for c in chars:
            src = open(os.path.join(psp_data, psp_name.format(c)), "rb").read()
            data = cache.value(f"files-{kind}", ["files"], [c, src, dc],  # from the cache when unchanged
                               lambda: conv(src, dc, c, dc_dir) if conv is pl_file else conv(src, dc))
            name = f"{kind}{c:02X}.PAC"
            open(os.path.join(out_dir, name), "wb").write(data)
            out.append((name, len(data)))
    return out


def main():
    psp_data, dc_dir, out_dir = sys.argv[1:4]
    chars = [int(a, 16) for a in sys.argv[4:]] or NEW
    for name, size in build(psp_data, dc_dir, out_dir, chars):
        print(f"{name}: {size} bytes")


if __name__ == "__main__":
    main()
