"""Throws on the PSP characters: where a Dreamcast character puts a held PSP
character.

A character's descriptor word 9 is a table by the opponent's character
number (38 entries, in its character file) of lists: where the held opponent
goes, frame by frame (f_8C05206E, f_8C0520F0, f_8C052180, f_8C052268). The
PSP's are 42 entries, built at load from the character's archive section 0x11
(a pool entry of 42 offsets), and its lists for the old opponents are the
Dreamcast's, byte for byte. So: for each Dreamcast character file, find the
pool entry whose first 38 lists are the Dreamcast table's, and take its last
4 lists. The runtime (psp_load.c) gives the attacker's descriptor a 42-entry
copy of its table when its opponent is a PSP character.
"""
import os
import struct

import build_char

POOL_DATA = 0x208          # section 0x11: the lists start here
NEW = range(0x26, 0x2A)


def overlay_table(path):
    """(the overlay's data, its word 9 table as offsets in the file, the list size)"""
    import sys
    sys.path.insert(0, os.path.join(build_char.HERE, "..", "recomp"))
    from align import Dc
    dc = Dc(path)
    dw = dc.descriptor()
    # word 9; for Gen (0x11) the descriptor align.Dc finds starts 5 words late: its word 4
    for w in (9, 4):
        t = dw[w] - build_char.SLOT_BASE
        if not 0 <= t <= len(dc.data) - 4 * 38:
            continue
        offs = [struct.unpack_from("<I", dc.data, t + 4 * k)[0] - build_char.SLOT_BASE for k in range(38)]
        if all(0 <= o < len(dc.data) for o in offs):
            gaps = [b - a for a, b in zip(sorted(set(offs)), sorted(set(offs))[1:])]
            return dc.data, offs, min(gaps)
    raise ValueError(f"{path}: no table of 38 lists at word 9")


def new_lists(dc_path, psp_pac):
    """the PSP's lists for the four new opponents (bytes each), or None. The
    pool entry is the one whose lists start as the Dreamcast table's do (all
    but a few: the PSP changed one of Guile's and Fei-Long's); the new lists
    follow one another, their size the distance between them."""
    data, offs, gap = overlay_table(dc_path)
    n = min(16, gap)
    sec = build_char.pac_sections(psp_pac)[0x11]
    best = (0, None)
    for idx in range(64):
        try:
            words = build_char.pool_words(sec, idx)
        except struct.error:
            break
        if len(words) != 42:
            continue
        same = sum(sec[POOL_DATA + w[1]:POOL_DATA + w[1] + n] == data[o:o + n] for w, o in zip(words, offs))
        if same > best[0]:
            best = (same, words)
    same, words = best
    if same < 34:
        return None
    distinct = sorted({w[1] for w in words})
    size = min(b - a for a, b in zip(distinct, distinct[1:]))
    if not 0 < size <= 0x400:
        raise ValueError(f"{psp_pac}: lists of {size} bytes?")
    return [sec[POOL_DATA + w[1]:POOL_DATA + w[1] + size] for w in words[38:]]


def build(dc_dir, psp_data):
    """{Dreamcast character file number: [4 lists]}"""
    out = {}
    for c in range(0x26):
        dc_path = os.path.join(dc_dir, f"PL{c:02X}A.BIN")
        psp_path = os.path.join(psp_data, f"pl{c:02x}_u.pac")
        if os.path.exists(dc_path) and os.path.exists(psp_path):
            lists = new_lists(dc_path, psp_path)
            if lists:
                out[c] = lists
    return out


def patches(dc_dir, psp_data, base):
    """[(address, bytes)]: at base a u32 [0x26][4] table of list addresses (0:
    none), the lists after it; the end"""
    lists = build(dc_dir, psp_data)
    table = bytearray(0x26 * 16)
    out, a = [], base + len(table)
    for c, four in sorted(lists.items()):
        for k, data in enumerate(four):
            struct.pack_into("<I", table, 16 * c + 4 * k, a)
            out.append((a, data))
            a += (len(data) + 3) & ~3
    return [(base, bytes(table))] + out, a


if __name__ == "__main__":
    import sys
    r = build(sys.argv[1], sys.argv[2])
    print(len(r), "characters:", " ".join(f"{c:02X}:{len(v[0])}" for c, v in sorted(r.items())))
