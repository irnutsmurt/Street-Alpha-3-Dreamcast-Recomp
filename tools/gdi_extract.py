#!/usr/bin/env python3
"""Extract the ISO9660 filesystem from a Dreamcast GDI's high-density data track,
and write a compact copy of the disc for an install.

Usage: gdi_extract.py <file.gdi> <out_dir>

GDI lines are "track lba type sector_size file offset", plus (only in the
compact images written here) a 7th column: the number of sectors. A track may
then have several lines, one per stretch of sectors stored; what lies between
them is never read and reads as zeros.
"""
import os
import re
import shutil
import struct
import sys


class Track:
    def __init__(self, path, lba, ttype, sector_size, offset=0, count=None, num=0):
        self.path, self.lba, self.ttype, self.sector_size, self.num = path, lba, ttype, sector_size, num
        self.offset = max(offset, 0)  # byte offset of the first sector (e.g. a Redump BIN's pregap)
        self.f = open(path, "rb")
        self.nsectors = count if count is not None else (os.path.getsize(path) - self.offset) // sector_size

    def read_sector(self, lba):
        rel = lba - self.lba
        if not 0 <= rel < self.nsectors:
            raise ValueError(f"LBA {lba} outside track {self.path}")
        self.f.seek(self.offset + rel * self.sector_size + (16 if self.sector_size == 2352 else 0))
        return self.f.read(2048).ljust(2048, b"\0")


_LINE = re.compile(r'^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+("[^"]*"|\S+)\s+(-?\d+)(?:\s+(\d+))?\s*$')


def parse_gdi(gdi_path):
    base = os.path.dirname(os.path.abspath(gdi_path))
    tracks = []
    for line in open(gdi_path).read().splitlines()[1:]:
        m = _LINE.match(line)
        if not m:
            continue
        num, lba, ttype, ssize = (int(m.group(i)) for i in range(1, 5))
        name = m.group(5).strip('"')
        count = int(m.group(7)) if m.group(7) else None
        tracks.append(Track(os.path.join(base, name), lba, ttype, ssize, int(m.group(6)), count, num))
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


def used_sectors(tracks):
    """The high-density sectors the game can read: the system area and volume
    descriptors, every directory and every file (as [first, end) LBA ranges)."""
    disc = Disc(tracks)
    hd = tracks[2].lba
    ranges = [(hd, hd + 32)]  # IP.BIN (16 sectors), the volume descriptors after it

    def walk_dirs(lba, size):
        ranges.append((lba, lba + (size + 2047) // 2048))
        data = disc.read(lba, size)
        off = 0
        while off < len(data):
            rlen = data[off]
            if rlen == 0:
                off = (off // 2048 + 1) * 2048
                continue
            rec = data[off:off + rlen]
            off += rlen
            ext_lba, ext_len = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            if rec[33:33 + rec[32]] in (b"\x00", b"\x01"):
                continue
            if rec[25] & 2:
                walk_dirs(ext_lba, ext_len)
            else:
                ranges.append((ext_lba, ext_lba + (ext_len + 2047) // 2048))

    pvd = disc.read(hd + 16, 2048)
    walk_dirs(struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0])
    merged = []
    for a, b in sorted(ranges):
        if merged and a <= merged[-1][1] + 64:  # small gaps are kept rather than split
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    return [tuple(r) for r in merged]


def write_compact(tracks, out_dir, progress=None):
    """A copy of the disc with only what the game reads: the low-density tracks as
    they are, and the high-density data as 2048-byte sectors, stretches of the disc
    with nothing in them left out. Returns the new disc.gdi (about 700 MB instead of 1.2 GB)."""
    os.makedirs(out_dir, exist_ok=True)
    disc = Disc(tracks)
    lines = []
    for i, t in enumerate(tracks[:2]):  # track 1 (data) and 2 (audio), a few MB
        name = f"track{i + 1:02d}" + (".bin" if t.ttype == 4 else ".raw")
        with open(t.path, "rb") as src, open(os.path.join(out_dir, name), "wb") as dst:
            src.seek(t.offset)
            remaining = t.nsectors * t.sector_size
            while remaining > 0:
                chunk = src.read(min(remaining, 1 << 20))
                if not chunk:
                    break
                dst.write(chunk)
                remaining -= len(chunk)
        lines.append(f"{i + 1} {t.lba} {t.ttype} {t.sector_size} {name} 0 {t.nsectors}")
    hd = tracks[2]
    hd_end = max(t.lba + t.nsectors for t in tracks[2:])
    ranges = used_sectors(tracks)
    ranges[-1] = (ranges[-1][0], max(ranges[-1][1], hd_end))  # the track's length as on the disc (the TOC)
    total = sum(min(b, hd_end) - a for a, b in ranges)
    done, pos = 0, 0
    with open(os.path.join(out_dir, "track03.bin"), "wb") as f:
        for a, b in ranges:
            lines.append(f"3 {a} {hd.ttype} 2048 track03.bin {pos * 2048} {b - a}")
            for lba in range(a, min(b, hd_end)):
                f.write(disc.read(lba, 2048))
                pos += 1
                done += 1
                if progress and done % 4096 == 0:
                    progress(done, total)
    with open(os.path.join(out_dir, "disc.gdi"), "w") as f:
        f.write(f"3\n" + "\n".join(lines) + "\n")
    return os.path.join(out_dir, "disc.gdi")


def main():
    extract(parse_gdi(sys.argv[1]), sys.argv[2])


if __name__ == "__main__":
    main()
