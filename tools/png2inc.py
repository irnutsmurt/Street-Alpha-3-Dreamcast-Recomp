#!/usr/bin/env python3
"""PNG -> C include of RGBA8 pixels, for the runtime's built-in images.

usage: png2inc.py <in.png> <out.inc> <name>
Writes `static const uint32_t <name>_w, <name>_h` and `<name>_rgba[]` (one
RGBA8 pixel per uint32, R in the low byte). Needs only the standard library:
8-bit greyscale / RGB / palette / RGBA, non-interlaced.
"""
import struct
import sys
import zlib


def decode(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat, plte, trns = 8, b"", None, None
    w = h = depth = ctype = interlace = None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            plte = body
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if depth != 8 or interlace:
        raise SystemExit(f"{path}: only 8-bit, non-interlaced PNGs are supported")
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * bpp
    prev = bytearray(stride)
    out = []
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        for x in range(w):
            px = line[x * bpp:(x + 1) * bpp]
            if ctype == 0:
                r = g = bl = px[0]; al = 255
            elif ctype == 4:
                r = g = bl = px[0]; al = px[1]
            elif ctype == 2:
                r, g, bl = px; al = 255
            elif ctype == 6:
                r, g, bl, al = px
            else:
                i = px[0]
                r, g, bl = plte[i * 3:i * 3 + 3]
                al = trns[i] if trns and i < len(trns) else 255
            out.append(r | g << 8 | bl << 16 | al << 24)
    return w, h, out


def main():
    src, dst, name = sys.argv[1:4]
    w, h, px = decode(src)
    with open(dst, "w") as f:
        f.write(f"/* generated from {src.split('/')[-1]} by tools/png2inc.py */\n")
        f.write(f"static const uint32_t {name}_w = {w}, {name}_h = {h};\n")
        f.write(f"static const uint32_t {name}_rgba[{w * h}] = {{\n")
        for i in range(0, len(px), 8):
            f.write("    " + ", ".join(f"0x{p:08X}u" for p in px[i:i + 8]) + ",\n")
        f.write("};\n")


if __name__ == "__main__":
    main()
