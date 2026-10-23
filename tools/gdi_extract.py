#!/usr/bin/env python3
"""Extract the ISO9660 filesystem from a Dreamcast GDI's high-density data track.

Usage: gdi_extract.py <file.gdi> <out_dir>
"""
import os
import struct
import sys


class Track:
    def __init__(self, path, lba, ttype, sector_size, offset=0):
        self.path, self.lba, self.ttype, self.sector_size = path, lba, ttype, sector_size
        self.offset = max(offset, 0)  # byte offset of the first sector (e.g. a Redump BIN's pregap)
        self.f = open(path, "rb")
        self.nsectors = (os.path.getsize(path) - self.offset) // sector_size

    def read_sector(self, lba):
        rel = lba - self.lba
        if not 0 <= rel < self.nsectors:
            raise ValueError(f"LBA {lba} outside track {self.path}")
        self.f.seek(self.offset + rel * self.sector_size + (16 if self.sector_size == 2352 else 0))
        return self.f.read(2048)


def parse_gdi(gdi_path):
    base = os.path.dirname(os.path.abspath(gdi_path))
    lines = [l.split() for l in open(gdi_path) if l.strip()]
    tracks = []
    for parts in lines[1:]:
        num, lba, ttype, ssize = map(int, parts[:4])
        name = " ".join(parts[4:-1]).strip('"')
        offset = int(parts[-1])
        tracks.append(Track(os.path.join(base, name), lba, ttype, ssize, offset))  # absolute names stay as is
    return tracks


class Disc:
    def __init__(self, tracks):
        self.tracks = tracks

    def read(self, lba, length):
        out = bytearray()
        while len(out) < length:
            trk = max((t for t in self.tracks if t.lba <= lba), key=lambda t: t.lba)
            out += trk.read_sector(lba)
            lba += 1
        return bytes(out[:length])


def walk(disc, lba, size, path, out_dir, listing):
    data = disc.read(lba, size)
    off = 0
    while off < len(data):
        rlen = data[off]
        if rlen == 0:  # records never span sectors; skip to next sector
            off = (off // 2048 + 1) * 2048
            continue
        rec = data[off:off + rlen]
        ext_lba = struct.unpack_from("<I", rec, 2)[0]
        ext_len = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        name_len = rec[32]
        name = rec[33:33 + name_len]
        off += rlen
        if name in (b"\x00", b"\x01"):
            continue
        name = name.decode("ascii").split(";")[0]
        full = os.path.join(path, name)
        if flags & 2:
            os.makedirs(os.path.join(out_dir, full), exist_ok=True)
            walk(disc, ext_lba, ext_len, full, out_dir, listing)
        else:
            listing.append((full, ext_lba, ext_len))
            with open(os.path.join(out_dir, full), "wb") as f:
                f.write(disc.read(ext_lba, ext_len))


def extract(tracks, out_dir):
    disc = Disc(tracks)
    hd = tracks[2]  # track 3: start of the high-density area
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "IP.BIN"), "wb") as f:
        f.write(disc.read(hd.lba, 0x8000))
    pvd = disc.read(hd.lba + 16, 2048)
    assert pvd[1:6] == b"CD001", "no ISO9660 PVD"
    root = pvd[156:156 + 34]
    root_lba = struct.unpack_from("<I", root, 2)[0]
    root_len = struct.unpack_from("<I", root, 10)[0]
    listing = []
    walk(disc, root_lba, root_len, "", out_dir, listing)
    with open(os.path.join(out_dir, "filelist.txt"), "w") as f:
        for name, lba, ln in listing:
            f.write(f"{lba:8d} {ln:10d} {name}\n")
    print(f"      extracted {len(listing)} files")
    return listing


def main():
    extract(parse_gdi(sys.argv[1]), sys.argv[2])


if __name__ == "__main__":
    main()
