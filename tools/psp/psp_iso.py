"""The player's PSP disc image (Street Fighter Alpha 3 MAX, USA) for the PSP characters add-on.

ISO 9660 as an .iso, a .cso (CISO: deflated 2048-byte blocks) or an .iso / .cso
inside a .zip. extract() writes what the add-on's tools read:
PSP_GAME/SYSDIR/BOOT.BIN (the program, checked against the known release) and
PSP_GAME/USRDIR/usa/data/ (the game's archives).

    python3 tools/psp/psp_iso.py <image> <out dir>
"""
import hashlib
import os
import shutil
import struct
import sys
import zipfile
import zlib

SECTOR = 2048
WANT = (("PSP_GAME", "SYSDIR", "BOOT.BIN"), ("PSP_GAME", "USRDIR", "usa", "data"))
KNOWN_BOOT = {"3fd0abf7cc65742233e1b7787997fa3e20152d98": "Street Fighter Alpha 3 MAX (USA)"}


class ImageError(Exception):
    pass


class Iso:
    def __init__(self, f):
        self.f = f

    def read(self, lba, size):
        self.f.seek(lba * SECTOR)
        return self.f.read(size)


class Ciso:
    """CISO: a header ("CISO", header size, total size, block size, version, align), then an index of
    u32 block offsets (<< align; bit 31: stored as is), the blocks raw-deflated"""
    def __init__(self, f):
        self.f = f
        magic, _, total, self.block, _, self.align = struct.unpack("<4sIQIBB", f.read(22))
        if magic != b"CISO" or self.block != SECTOR:
            raise ImageError("not a CSO image this can read")
        n = (total + self.block - 1) // self.block
        f.seek(24)
        self.index = struct.unpack(f"<{n + 1}I", f.read(4 * (n + 1)))

    def block_data(self, k):
        a, b = self.index[k], self.index[k + 1]
        start, end = (a & 0x7FFFFFFF) << self.align, (b & 0x7FFFFFFF) << self.align
        self.f.seek(start)
        raw = self.f.read(end - start)
        return raw[:SECTOR] if a & 0x80000000 else zlib.decompressobj(-15).decompress(raw)[:SECTOR]

    def read(self, lba, size):
        out = bytearray()
        while len(out) < size:
            out += self.block_data(lba)
            lba += 1
        return bytes(out[:size])


def entries(img, lba, size):
    """a directory's (name, lba, size, is directory)"""
    data = img.read(lba, size)
    out, p = [], 0
    while p < len(data):
        n = data[p]
        if n == 0:  # records don't cross sectors: the rest of this one is empty
            p = (p // SECTOR + 1) * SECTOR
            continue
        rec = data[p:p + n]
        name = rec[33:33 + rec[32]]
        if name not in (b"\0", b"\1"):
            out.append((name.decode("ascii", "replace").split(";")[0], struct.unpack_from("<I", rec, 2)[0],
                        struct.unpack_from("<I", rec, 10)[0], bool(rec[25] & 2)))
        p += n
    return out


def find(img, root, path):
    lba, size, is_dir = root
    for part in path:
        for name, l, s, d in entries(img, lba, size):
            if name.upper() == part.upper():
                lba, size, is_dir = l, s, d
                break
        else:
            return None
    return lba, size, is_dir


def extract_from(img, out_dir, progress=None):
    pvd = img.read(16, SECTOR)
    if pvd[1:6] != b"CD001":
        raise ImageError("not a disc image (no ISO 9660 volume)")
    root = (struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0], True)
    jobs = []
    for want in WANT:
        hit = find(img, root, want)
        if not hit:
            raise ImageError("not the PSP game this needs (no " + "/".join(want) + ")")
        lba, size, is_dir = hit
        if is_dir:
            jobs += [(os.path.join(out_dir, *want, n), l, s) for n, l, s, d in entries(img, lba, size) if not d]
        else:
            jobs.append((os.path.join(out_dir, *want), lba, size))
    total, done = sum(s for _, _, s in jobs), 0
    for path, lba, size in jobs:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            for k in range(0, size, 64 * SECTOR):
                f.write(img.read(lba + k // SECTOR, min(64 * SECTOR, size - k)))
                done += min(64 * SECTOR, size - k)
                if progress:
                    progress(done, total)
    boot = os.path.join(out_dir, *WANT[0])
    h = hashlib.sha1(open(boot, "rb").read()).hexdigest()
    if h not in KNOWN_BOOT:
        raise ImageError("this PSP disc is not the supported release (Street Fighter Alpha 3 MAX, USA)")
    return KNOWN_BOOT[h]


def extract(image, out_dir, progress=None):
    """image's files the add-on needs, to out_dir (written afresh): the release's name"""
    shutil.rmtree(out_dir, ignore_errors=True)
    os.makedirs(out_dir)
    if zipfile.is_zipfile(image):
        with zipfile.ZipFile(image) as z:
            members = [m for m in z.infolist() if m.filename.lower().endswith((".iso", ".cso"))]
            if not members:
                raise ImageError("no .iso or .cso in the zip")
            m = max(members, key=lambda m: m.file_size)
            tmp = os.path.join(out_dir, "image" + os.path.splitext(m.filename)[1].lower())
            with z.open(m) as src, open(tmp, "wb") as dst:
                done = 0
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    dst.write(chunk)
                    done += len(chunk)
                    if progress:
                        progress(done, 2 * m.file_size)  # the unzipping: the first half
        try:
            return extract(tmp, out_dir + ".iso", progress and (lambda d, t: progress(t + d, 2 * t)))
        finally:
            os.remove(tmp)
            for name in os.listdir(out_dir + ".iso"):
                shutil.move(os.path.join(out_dir + ".iso", name), os.path.join(out_dir, name))
            shutil.rmtree(out_dir + ".iso", ignore_errors=True)
    with open(image, "rb") as f:
        head = f.read(4)
        f.seek(0)
        return extract_from(Ciso(f) if head == b"CISO" else Iso(f), out_dir, progress)


if __name__ == "__main__":
    print(extract(sys.argv[1], sys.argv[2], lambda d, t: None))
