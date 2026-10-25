"""Sprite banks in archives (PAC): parts and their tiles, read from the PSP's
files and added to the Dreamcast's.

A bank is two sections: parts (tag 0x01000000 | bank) and tiles
(0x02000000 | bank), sometimes palettes (0x04000000 | bank).
  parts:  12-byte records (u32 offset of the part's tile list, u8 width,
          u8 height in tiles, 6 more bytes kept as they are); then the tile
          lists, 3 bytes a tile (u16 big-endian tile number, attribute: the
          palette in its low 5 bits), FF for an empty one. The first record's
          offset is also the size of the records: the record count.
  tiles:  u32 offsets, then each tile LZ-packed (pac.unlz): 16x16 pixels, 4
          bits each, twiddled on the Dreamcast, row by row on the PSP.
Some banks (bank B of SELECT.PAC) draw 32x32 tiles: tile n is the four
16x16 tiles 4n..4n+3 (`group` 4). A part's tiles go row by row.

Appending keeps the Dreamcast's parts, tile numbers and tile data as they
are: new parts get the numbers after the last, new tiles the numbers after
the last (only the tables grow, so their offsets shift).
"""
import struct

import pac


class Bank:
    def __init__(self, parts_sec, tiles_sec, dc, group=1):
        self.dc, self.group = dc, group
        n = struct.unpack_from("<I", parts_sec, 0)[0] // 12
        self.records = []   # (12-byte record, [(tile number, attr) | None])
        for k in range(n):
            rec = parts_sec[12 * k:12 * k + 12]
            o, w, h = struct.unpack_from("<IBB", rec, 0)
            p, tl = o, []
            for _ in range(w * h):
                if parts_sec[p] == 0xFF:
                    tl.append(None)
                    p += 1
                    continue
                tl.append((parts_sec[p] << 8 | parts_sec[p + 1], parts_sec[p + 2]))
                p += 3
            self.records.append((bytes(rec), tl))
        self.parts_sec, self.n_orig = parts_sec, n
        self.tiles_sec = tiles_sec
        self.table = pac.tile_table(tiles_sec)
        self.new_tiles = []  # packed data of tiles added (Dreamcast form)
        self._copied = {}    # (source bank, tile) -> tile number here

    def tile_pixels(self, i):
        """16x16 tile i, row by row (linear)"""
        t = pac.unlz(self.tiles_sec, self.table[i])
        return pac.untwiddle(t) if self.dc else t

    def tile_count(self):
        return len(self.table) + len(self.new_tiles)

    def add_part(self, src, k, attr_map=None):
        """part k of bank src (PSP) added here; its tiles copied: the new part number"""
        rec, tl = src.records[k]
        new_tl = []
        for e in tl:
            if e is None:
                new_tl.append(None)
                continue
            t, attr = e
            if (src, t) not in self._copied:
                assert self.tile_count() % self.group == 0
                n = self.tile_count() // self.group
                for q in range(src.group):
                    lin = src.tile_pixels(src.group * t + q)
                    self.new_tiles.append(pac.lz_opt(pac.twiddle(lin)))
                self._copied[(src, t)] = n
            new_tl.append((self._copied[(src, t)], attr_map(attr) if attr_map else attr))
        self.records.append((rec, new_tl))
        return len(self.records) - 1

    def encode(self):
        """(parts section, tiles section): the original bytes kept, the new parts and tiles after them"""
        n0, n = self.n_orig, len(self.records)
        grow = 12 * (n - n0)
        recs = bytearray(self.parts_sec[:12 * n0])
        for k in range(n0):
            struct.pack_into("<I", recs, 12 * k, struct.unpack_from("<I", recs, 12 * k)[0] + grow)
        body = bytearray(self.parts_sec[12 * n0:])
        for rec, tl in self.records[n0:]:
            r = bytearray(rec)
            struct.pack_into("<I", r, 0, 12 * n + len(body))
            recs += r
            for e in tl:
                body += b"\xff" if e is None else bytes([e[0] >> 8, e[0] & 0xFF, e[1]])
        total = len(self.table) + len(self.new_tiles)
        offs = [o + 4 * len(self.new_tiles) for o in self.table]
        data = bytearray(self.tiles_sec[4 * len(self.table):])
        for t in self.new_tiles:
            offs.append(4 * total + len(data))
            data += t
        return bytes(recs + body), struct.pack(f"<{total}I", *offs) + bytes(data)
