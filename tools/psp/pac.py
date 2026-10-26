"""Character archives (PLnn.PAC) and their sprite tiles, PSP and Dreamcast.

An archive: u32 section count, u32 offset of the last section, then (tag, size)
pairs from 0x8; the header is 0x800 bytes and each section 0x800-aligned.
Sections: 0x100000 sprite parts, 0x200000 sprite tiles, 0x400000 palettes,
then sound and other data. A tile is 16x16 pixels, 4 bits each, LZ-packed
(unlz / lz_opt); the Dreamcast stores the pixels twiddled for the PowerVR, the
PSP row by row. Palettes: PSP ABGR1555, Dreamcast ARGB1555.
convert_tiles and lz_opt run in C when the add-on's library is there (native.py).
"""
import struct

import native

def sections(d):
    n=struct.unpack_from("<I",d,0)[0]; off=0x800; out={}
    for k in range(n):
        t,s=struct.unpack_from("<II",d,8+8*k)
        out[t]=d[off:off+s]; off=(off+s+0x7ff)&~0x7ff
    return out

def order(d):
    n=struct.unpack_from("<I",d,0)[0]
    return [struct.unpack_from("<I",d,8+8*k)[0] for k in range(n)]

def build(tags, secs):
    hdr=bytearray(0x800); body=bytearray(); off=0x800; last=0
    struct.pack_into("<I",hdr,0,len(tags))
    for k,t in enumerate(tags):
        x=secs[t]; last=off
        struct.pack_into("<II",hdr,8+8*k,t,len(x))
        body+=x; pad=(-len(x))&0x7ff; body+=bytes(pad); off+=len(x)+pad
    struct.pack_into("<I",hdr,4,last)
    return bytes(hdr+body)

def tile_table(sec):
    n=struct.unpack_from("<I",sec,0)[0]//4
    return [struct.unpack_from("<I",sec,4*k)[0] for k in range(n)]

def unlz(src, pos=0, size=128):
    out=bytearray(); flags=0; nb=0
    while len(out) < size:
        if nb==0: flags=src[pos]; pos+=1; nb=8
        if flags & 1:
            b=src[pos]; pos+=1; ln=b&15; d=b>>4
            if d==0:
                v=src[pos]; pos+=1; out+=bytes([v])*(ln+1)
            else:
                for _ in range(ln): out.append(out[-d])
        else:
            out.append(src[pos]); pos+=1
        flags>>=1; nb-=1
    return bytes(out[:size])

def twid(x, y):  # PowerVR twiddle: y in the even bits, x in the odd
    r = 0
    for b in range(4):
        r |= ((y >> b) & 1) << (2*b) | ((x >> b) & 1) << (2*b+1)
    return r

def px(t, i): return (t[i>>1] >> (4*(i&1))) & 15

def untwiddle(t):
    if native.LIB:
        return native.untwiddle(t)
    out=bytearray(128)
    for y in range(16):
        for x in range(16):
            v=px(t, twid(x,y)); i=y*16+x
            out[i>>1] |= v << (4*(i&1))
    return bytes(out)

def twiddle(lin):
    if native.LIB:
        return native.twiddle(lin)
    out=bytearray(128)
    for y in range(16):
        for x in range(16):
            v=px(lin, y*16+x); i=twid(x,y)
            out[i>>1] |= v << (4*(i&1))
    return bytes(out)

def show(*tiles):
    for r in range(16):
        print(" | ".join("".join(" .:-=+*#%@abcdef"[px(t, r*16+c)] for c in range(16)) for t in tiles))

def lz(data):
    """compress for unlz: greedy, back-copies up to 15 back / 15 long, runs up to 16"""
    out=bytearray(); i=0; n=len(data)
    while i < n:
        flag_pos=len(out); out.append(0); flags=0
        for bit in range(8):
            if i >= n: break
            # run of the same byte
            run=1
            while i+run<n and run<16 and data[i+run]==data[i]: run+=1
            best_l=0; best_d=0
            for d in range(1, min(15, i)+1):
                l=0
                while l<15 and i+l<n and data[i+l]==data[i+l-d]: l+=1
                if l>best_l: best_l, best_d = l, d
            if best_l>=2 and best_l>=run:
                out.append(best_d<<4 | best_l); i+=best_l; flags|=1<<bit
            elif run>=3:
                out.append(run-1); out.append(data[i]); i+=run; flags|=1<<bit
            else:
                out.append(data[i]); i+=1
        out[flag_pos]=flags
    return bytes(out)

def convert_tiles(sec):
    """a PSP sprite section (linear tiles) as the Dreamcast's (twiddled)"""
    if native.LIB:
        return native.convert_tiles(sec)
    t=tile_table(sec); body=bytearray(); offs=[]
    base=4*len(t)
    for k in range(len(t)):
        offs.append(base+len(body)); body+=lz_opt(twiddle(unlz(sec,t[k])))
    return struct.pack("<%dI"%len(offs),*offs)+bytes(body)

def convert_texture(psp, dc):
    """a PSP texture section (32-byte header, 256x256 pixels linear, ABGR1555) as the Dreamcast's
    (dc: one of its sections, for the header; the pixels twiddled, ARGB1555)"""
    side = 256
    tw = [0] * side  # the twiddle's bits of one coordinate: y in the even bits, x in the odd
    for v in range(side):
        tw[v] = sum(((v >> b) & 1) << (2 * b) for b in range(8))
    src = swap_rb(psp[32:32 + 2 * side * side])
    out = bytearray(dc[:32] + bytes(2 * side * side))
    for y in range(side):
        for x in range(side):
            i = 32 + 2 * (tw[y] | tw[x] << 1)
            out[i:i + 2] = src[2 * (y * side + x):2 * (y * side + x) + 2]
    return bytes(out)

def swap_rb(pal):
    out=bytearray(pal)
    for k in range(0,len(pal)-1,2):
        v=struct.unpack_from("<H",pal,k)[0]
        v=(v&0x83E0)|((v&0x1F)<<10)|((v>>10)&0x1F)
        struct.pack_into("<H",out,k,v)
    return bytes(out)

def lz_opt(data):
    """smallest encoding for unlz: dynamic programming over the positions.
    cost in bits: a literal 1+8, a back copy 1+8, a run 1+16"""
    if native.LIB:
        return native.lz_opt(data)
    n = len(data)
    INF = 1 << 30
    best = [INF] * (n + 1); how = [None] * (n + 1)
    best[n] = 0
    for i in range(n - 1, -1, -1):
        # literal
        c = 9 + best[i + 1]; h = ("L",)
        # runs: data[i] repeated (k+1) times, k 0..15
        run = 1
        while i + run < n and run < 16 and data[i + run] == data[i]:
            run += 1
        for L in range(1, run + 1):
            cc = 17 + best[i + L]
            if cc < c: c, h = cc, ("R", L)
        # back copies: distance 1..15, length 1..15 (the copy may overlap itself)
        for d in range(1, min(15, i) + 1):
            L = 0
            while L < 15 and i + L < n and data[i + L] == data[i + L - d]:
                L += 1
                cc = 9 + best[i + L]
                if cc < c: c, h = cc, ("C", d, L)
        best[i], how[i] = c, h
    out = bytearray(); items = []
    i = 0
    while i < n:
        h = how[i]; items.append(h)
        i += 1 if h[0] == "L" else (h[1] if h[0] == "R" else h[2])
    i = 0
    for k in range(0, len(items), 8):
        flag_pos = len(out); out.append(0); flags = 0
        for bit, h in enumerate(items[k:k + 8]):
            if h[0] == "L":
                out.append(data[i]); i += 1
            elif h[0] == "R":
                flags |= 1 << bit; out.append(h[1] - 1); out.append(data[i]); i += h[1]
            else:
                flags |= 1 << bit; out.append(h[1] << 4 | h[2]); i += h[2]
        out[flag_pos] = flags
    return bytes(out)
