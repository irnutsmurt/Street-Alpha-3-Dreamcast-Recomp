"""The ranking's portraits for the PSP characters: RK26-29.PAC and RK_COM.PAC.

After a game that makes the ranking, a screen (the task 8C061C7E, its step
8C062072) shows the new entry with its character: f_8C01C8D0 loads
RK_COM.PAC (sprite bank 0x16: the characters' name plates, part
8C0D2035[character] + 2) and names[90 + position] (RKnn.PAC, bank 0x17), and
8C07EAB4 draws them; two animation tables by character (8C0D1EA8, 8C0D1F40:
8-byte records) run the small sprites. A PSP character had none of these:
its rk name was the next family's file (an OP file).

RKnn.PAC: bank 0x17 (parts, tiles, palettes). The PSP's files are the
Dreamcast's without their first parts: the Dreamcast's start with the
portrait a second time (192 right and 96 down), each sprite frame alone and
each name strip alone (Ryu's 16 parts, Ken's 19, Akuma's 11); then the PSP's
order: the portrait, then (name strip, frame) pairs, which the animations
name (the PSP's tables 0x236DA8 and 0x236E50: the same records with part
numbers less the Dreamcast's lead). The new characters' files get that lead
made the same way. The palettes: the Dreamcast's 176 are the PSP's 194 with
groups of the PSP's left out (the same for every character; learnt from the
characters both have), colour 0 cleared (the PSP keeps a tag there).

RK_COM.PAC: the PSP's has the new characters' name plates after the others
(its part 2 + its table 0x2C8C36): added to the Dreamcast's bank.
"""
import os
import struct
from collections import Counter

import chartables
import pac
import sprites

NEW = (0x26, 0x27, 0x28, 0x29)
RK, RK_COM = 0x17, 0x16                    # the banks (section tags 0x1000000 / 0x2000000 / 0x4000000 + bank)
PLATES, PSP_PLATES = 0x8C0D2035, 0x2C8C36  # a byte per character: its name plate is RK_COM part + 2
ANIMS = ((0x8C0D1EA8, 0x236DA8), (0x8C0D1F40, 0x236DA8 + 4 * 42))  # Dreamcast table, the PSP's (42)
SHIFT = (192, 96)                          # the lead's portrait from the PSP's
DC_CHARS = [c for c in range(0x25) if c != 0x13]  # RKnn by character (0x13, the second Gen, uses 0x11's)


def records(parts):
    """[(w, h, x, y, next, tile list bytes)] of a parts section"""
    n = struct.unpack_from("<I", parts, 0)[0] // 12
    out = []
    for k in range(n):
        o, w, h, x, y, nxt = struct.unpack_from("<IBBhhH", parts, 12 * k)
        p = o
        for _ in range(w * h):
            p += 1 if parts[p] == 0xFF else 3
        out.append((w, h, x, y, nxt, parts[o:p]))
    return out


def encode(recs):
    head, body = bytearray(), bytearray()
    for w, h, x, y, nxt, tl in recs:
        head += struct.pack("<IBBhhH", 12 * len(recs) + len(body), w, h, x, y, nxt)
        body += tl
    return bytes(head + body)


def lead(recs):
    """the Dreamcast's parts before the PSP's: the portrait again, each frame alone, each name strip alone"""
    k = 0
    while recs[k][4]:
        k += 1
    portrait = recs[:k + 1]
    out = [(w, h, x + SHIFT[0], y + SHIFT[1], n, tl) for w, h, x, y, n, tl in portrait]
    pairs = [(recs[i], recs[i + 1]) for i in range(k + 1, len(recs) - 1, 2)]
    out += [f for _, f in pairs]
    strips = []
    for s, _ in pairs:
        if s[:4] + s[5:] not in [t[:4] + t[5:] for t in strips]:
            strips.append(s)
    out += [(w, h, x, y, 0, tl) for w, h, x, y, _, tl in strips]
    return out


def palette_map(dc_dir, psp_data):
    """Dreamcast palette index -> the PSP's (None: none), learnt from the characters both have"""
    votes = [Counter() for _ in range(176)]
    for c in DC_CHARS:
        d = pac.sections(open(os.path.join(dc_dir, f"RK{c:02X}.PAC"), "rb").read())[0x4000000 + RK]
        p = pac.swap_rb(pac.sections(open(os.path.join(psp_data, f"rk{c:02x}.pac"), "rb").read())[0x4000000 + RK])
        for k in range(176):
            want = d[32 * k + 2:32 * k + 32]
            if not any(want):
                continue
            for j in range(len(p) // 32):
                if p[32 * j + 2:32 * j + 32] == want:
                    votes[k][j - k] += 1
    return [k + v.most_common(1)[0][0] if v else None for k, v in enumerate(votes)]


def rk_file(psp_rk, template, pal_map):
    """(RKnn.PAC the Dreamcast's way, the lead's length)"""
    s, t = pac.sections(psp_rk), pac.sections(template)
    recs = records(s[0x1000000 + RK])
    ld = lead(recs)
    t[0x1000000 + RK] = encode(ld + recs)
    t[0x2000000 + RK] = pac.convert_tiles(s[0x2000000 + RK])
    p, out = pac.swap_rb(s[0x4000000 + RK]), bytearray(t[0x4000000 + RK])
    for k, j in enumerate(pal_map):
        if j is not None and 32 * j + 32 <= len(p):
            out[32 * k:32 * k + 32] = b"\0\0" + p[32 * j + 2:32 * j + 32]
    t[0x4000000 + RK] = bytes(out)
    return pac.build(pac.order(template), t), len(ld)


def anim(boot, a, shift):
    """an animation's 8-byte records from the PSP (to the one flagged last), part numbers moved by shift"""
    out = bytearray()
    while True:
        r = bytearray(boot[a + 0x74:a + 0x74 + 8])
        struct.pack_into("<H", r, 4, struct.unpack_from("<H", r, 4)[0] + shift)
        out += r
        a += 8
        if r[1] & 0x80:
            return bytes(out)


def build(dc_dir, psp_data, boot, data):
    """{file name: bytes}, [(address, bytes)] patches (the records at data on)"""
    tables = {t.dc: t for t in chartables.layout()}
    files, patches = {}, []
    template = open(os.path.join(dc_dir, "RK00.PAC"), "rb").read()
    pal_map = palette_map(dc_dir, psp_data)
    for c in NEW:
        f, shift = rk_file(open(os.path.join(psp_data, f"rk{c:02x}.pac"), "rb").read(), template, pal_map)
        files[f"RK{c:02X}.PAC"] = f
        for dc_table, psp_table in ANIMS:
            recs = anim(boot, struct.unpack_from("<I", boot, psp_table + 0x74 + 4 * c)[0], shift)
            patches += [(data, recs), (tables[dc_table].ext + 4 * c, struct.pack("<I", data))]
            data += len(recs)
    d = pac.sections(open(os.path.join(dc_dir, "RK_COM.PAC"), "rb").read())
    p = pac.sections(open(os.path.join(psp_data, "rkcom_u.pac"), "rb").read())
    db = sprites.Bank(d[0x1000000 + RK_COM], d[0x2000000 + RK_COM], True)
    pb = sprites.Bank(p[0x1000000 + RK_COM], p[0x2000000 + RK_COM], False)
    for c in NEW:
        k = db.add_part(pb, 2 + boot[PSP_PLATES + 0x74 + c])
        patches.append((tables[PLATES].ext + c, bytes([k - 2])))
    d[0x1000000 + RK_COM], d[0x2000000 + RK_COM] = db.encode()
    files["RK_COM.PAC"] = pac.build(pac.order(open(os.path.join(dc_dir, "RK_COM.PAC"), "rb").read()), d)
    return files, patches, data
