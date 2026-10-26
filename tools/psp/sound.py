"""Sound: the PSP's voice banks in the Dreamcast's form.

The PSP's (a character file's sections 0x40000002, 0x40008002):
  PPHD  the bank's header: at +0x18 the offset of its PPVA chunk
  PPVA  u32 size, u32 16 (an entry's size), -1, 0, u32 entries - 1, -1, -1,
        then per sample: u32 offset in the VAG data, u32 sample rate, u32
        size in bytes, -1
  VAG   Sony ADPCM: 16-byte frames (shift and filter, flags, 28 4-bit samples)

The Dreamcast's (sound driver MANATEE; Katana one-shot bank and MIDI program
bank):
  SOSB  "SOSB", 1, u32 size, u32 count, count u32 offsets of 0x38-byte SOSP
        records (an AICA voice's registers: +4 address bits 16-23, +5 format,
        +6 address bits 0-15, +8 loop start, +0xA end (samples), +0x10 pitch,
        +0x2C/+0x30 loop start and end again; "ENDP" at +0x34), then the
        samples: AICA 4-bit ADPCM, two samples a byte, the low nibble first
  SMPB  the KO echo's instrument bank: one sample, the voice bank's first

The sample numbers line up: every character the two have in common has as
many samples on the PSP as on the Dreamcast, in the same order, and the
voice records (section 0xD, 64 x 16 bytes) are the same format on both.

Level: the PSP's samples are about 1.53 times as loud as the Dreamcast's
for the same voices; the new characters' are scaled by GAIN to match.

Room: the sound memory map (ZERO3.MLT) gives each player's voice bank
0x48000 bytes and its KO echo bank 0x5000; Maki's, Yun's and Ingrid's
voices are bigger, so their samples are cut to their sound (trailing
silence, the VAG's silent first frame) and, as far as it takes, played at a
lower rate. The KO scream (sample 0) is kept to 1.8 s: the echo bank holds
it too. The AICA's sample end is 16 bits: a sample is at most 65535 long.

Records of type 4 (the PSP's sound queue, 0x5A4C, turns them into type 0 on
the channel + 8, a nonzero bank + 2) are written as type 0 on a channel of
the player's (0-2: the per-side base, 8C0BA64C, is 0 or 3) that the
character's other records don't use; when the character's type 4 records
only stop (Ingrid's), they become none (FFFF; their next record still
plays). The Dreamcast's queue (f_8C01424C) waits forever on a type it
doesn't know.
"""
import math
import struct

VAG_FILTERS = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))
AICA_SCALE = (230, 230, 230, 230, 307, 409, 512, 614)
AICA_RATE = 44100


def vag_decode(data):
    """Sony ADPCM frames to 16-bit samples (stops at a frame flagged as the end)"""
    out, s1, s2 = [], 0, 0
    for o in range(0, len(data) - 15, 16):
        head, flags = data[o], data[o + 1]
        shift, f = head & 15, head >> 4
        f0, f1 = VAG_FILTERS[f] if f < 5 else (0, 0)
        for k in range(28):
            b = data[o + 2 + k // 2]
            n = (b >> 4) if k & 1 else (b & 15)
            v = (n - 16 if n & 8 else n) << 12
            v >>= shift
            v += (s1 * f0 + s2 * f1 + 32) >> 6
            v = max(-32768, min(32767, v))
            s2, s1 = s1, v
            out.append(v)
        if flags & 1:
            break
    return out


def aica_decode(data, count):
    out, cur, step = [], 0, 127
    for n in range(count):
        b = data[n >> 1]
        nib = (b >> 4) if n & 1 else (b & 15)
        delta = (((nib & 7) * 2 + 1) * step) >> 3
        cur = max(-32768, min(32767, cur - delta if nib & 8 else cur + delta))
        step = max(127, min(24576, (step * AICA_SCALE[nib & 7]) >> 8))
        out.append(cur)
    return out


def aica_encode(samples):
    """16-bit samples to AICA ADPCM (the runtime's and the chip's decoder):
    each nibble the one whose decoded value comes nearest"""
    out = bytearray((len(samples) + 1) // 2)
    cur, step = 0, 127
    for n, x in enumerate(samples):
        d = x - cur
        mag = min(7, (abs(d) * 8 // step) // 2) if step else 0
        best = None
        for m in {max(0, mag - 1), mag, min(7, mag + 1)}:
            delta = ((m * 2 + 1) * step) >> 3
            v = max(-32768, min(32767, cur - delta if d < 0 else cur + delta))
            if best is None or abs(x - v) < abs(x - best[1]):
                best = (m, v)
        m, cur = best
        nib = m | (8 if d < 0 else 0)
        step = max(127, min(24576, (step * AICA_SCALE[m]) >> 8))
        out[n >> 1] |= nib << 4 if n & 1 else nib
    return bytes(out)


def pitch(rate):
    """the AICA's OCT/FNS for playing a sample of this rate at its speed"""
    r = rate / AICA_RATE
    octave = math.floor(math.log2(r))
    fns = round((r / 2 ** octave - 1) * 1024)
    if fns == 1024:
        octave, fns = octave + 1, 0
    return (octave & 15) << 11 | fns


def psp_bank(pphd, vag):
    """[(rate, samples)] of a PSP voice bank"""
    pva = struct.unpack_from("<I", pphd, 0x18)[0]
    assert pphd[pva:pva + 4] == b"PPVA", pphd[pva:pva + 4]
    count = struct.unpack_from("<I", pphd, pva + 0x14)[0] + 1
    out = []
    for k in range(count):
        off, rate, size = struct.unpack_from("<III", pphd, pva + 0x20 + 16 * k)
        out.append((rate, vag_decode(vag[off:off + size])))
    return out


def dc_bank(sosb):
    """[(pitch, samples)] of a Dreamcast voice bank"""
    count = struct.unpack_from("<I", sosb, 12)[0]
    out = []
    for o in struct.unpack_from(f"<{count}I", sosb, 16):
        rec = sosb[o:o + 0x38]
        assert rec[:4] == b"SOSP"
        addr = rec[4] << 16 | struct.unpack_from("<H", rec, 6)[0]
        end = struct.unpack_from("<H", rec, 0xA)[0]
        out.append((struct.unpack_from("<H", rec, 0x10)[0], aica_decode(sosb[addr:], end)))
    return out


SLOT_SOSB, SLOT_SMPB = 0x48000, 0x5000  # ZERO3.MLT: a player's voice bank, KO echo bank
MAX_LEN = 0xFFFF                         # the AICA's sample end register
KO_MAX = 39690                           # the KO scream: 1.8 s at 22050 Hz
SILENCE = 48                             # below this a sample's end is cut
PAD = 16                                 # after each sample (the AICA may read ahead)
SMPB_SPLIT, SMPB_DATA = 0xEC, 0x11C      # the template's split record and its sample
RECORD, SOSB_HEAD = 0x38, 16
BLOCK = 32  # banks go to the sound memory in whole blocks: a bank's section is padded to them (its size field isn't)


def resample(x, ratio, taps=8, phases=128):
    """x at ratio times its rate (ratio < 1): windowed-sinc low-pass"""
    if ratio >= 1:
        return list(x)
    cut = ratio * 0.92
    half = int(math.ceil(taps / cut))
    table = []
    for p in range(phases):
        frac, row = p / phases, []
        for k in range(-half + 1, half + 1):
            d = k - frac
            w = 0.5 + 0.5 * math.cos(math.pi * d / half)
            row.append(w * (cut * math.sin(math.pi * cut * d) / (math.pi * cut * d) if d else cut))
        table.append(row)
    xp = [0] * half + list(x) + [0] * half
    out = []
    for j in range(int(len(x) * ratio)):
        t = j / ratio
        i = int(t)
        v = sum(a * b for a, b in zip(xp[i + 1:i + 1 + 2 * half], table[int((t - i) * phases)]))
        out.append(max(-32768, min(32767, int(v))))
    return out


def fade_cut(x, n, fade=3300):
    """x cut to n samples, the last fade of them faded out"""
    if len(x) <= n:
        return x
    x = list(x[:n])
    for i in range(fade):
        x[n - fade + i] = x[n - fade + i] * (fade - i) // fade
    return x


def trimmed(rate, x, first):
    """a PSP sample cut to its sound: (rate, samples)"""
    if rate == 0xFFFFFFFF or not x:  # an empty entry: keep the numbering
        return 22050, [0] * 32
    if len(x) > 28 and not any(x[:28]):
        x = x[28:]
    end = len(x)
    while end > 32 and abs(x[end - 1]) < SILENCE:
        end -= 1
    x = x[:end]
    return rate, fade_cut(x, KO_MAX) if first else x


def stored(n):
    return ((n + 1) // 2 + 3 & ~3) + PAD


def fit(samples):
    """the rate factor (for samples above 12 kHz) that fits the voice bank"""
    head = SOSB_HEAD + (4 + RECORD) * len(samples) + 4 + 8 + BLOCK

    def ratio(rate, n, f):
        r = f if rate > 12000 else 1.0
        return min(r, MAX_LEN / n)

    f = 1.0
    while head + sum(stored(int(len(x) * ratio(r, len(x), f))) for r, x in samples) > SLOT_SOSB:
        f -= 0.005
    return [ratio(r, len(x), f) for r, x in samples], f


# The PSP's voices are louder than the Dreamcast's: for the characters both
# have, a sample's RMS on the PSP is 1.53 times the Dreamcast's (the median;
# Ryu, Ken, Sagat, Juli alike). The new characters' are scaled to match.
GAIN = 1 / 1.53


def voice_bank(pphd, vag, sosb_template, smpb_template):
    """(SOSB, SMPB, [(rate, length)]) for a PSP character's voice bank"""
    samples = [trimmed(rate, [int(round(v * GAIN)) for v in x], k == 0) for k, (rate, x) in enumerate(psp_bank(pphd, vag))]
    ratios, f = fit(samples)
    n = len(samples)
    rec0 = struct.unpack_from("<I", sosb_template, SOSB_HEAD)[0]
    template = sosb_template[rec0:rec0 + RECORD]
    assert template[:4] == b"SOSP" and template[-4:] == b"ENDP"
    data, records, info = bytearray(), [], []
    base = SOSB_HEAD + (4 + RECORD) * n + 4
    for (rate, x), r in zip(samples, ratios):
        if r < 1:
            x, rate = resample(x, r), int(round(rate * r))
        enc = aica_encode(x)
        addr = base + len(data)
        rec = bytearray(template)
        rec[4], rec[5] = addr >> 16, 0x01  # ADPCM, no loop
        struct.pack_into("<HHH", rec, 6, addr & 0xFFFF, 0, len(x))
        struct.pack_into("<H", rec, 0x10, pitch(rate))
        struct.pack_into("<H", rec, 0x2C, 0)
        struct.pack_into("<H", rec, 0x30, len(x))
        records.append(bytes(rec))
        data += enc + b"\x80" * (stored(len(x)) - len(enc))
        info.append((rate, len(x)))
    head = bytearray(b"SOSB" + struct.pack("<III", 1, 0, n))
    head += struct.pack(f"<{n}I", *(SOSB_HEAD + 4 * n + RECORD * k for k in range(n)))
    sosb = head + b"".join(records) + b"SOSD" + data + b"ENDDENDB"
    struct.pack_into("<I", sosb, 8, len(sosb))
    sosb += bytes(-len(sosb) % BLOCK)
    assert len(sosb) <= SLOT_SOSB, len(sosb)
    # the KO echo bank: the template's program with the KO scream at 22050 Hz
    rate, ko = samples[0]
    if rate != 22050:
        ko = resample(ko, 22050 / rate) if rate > 22050 else ko
    enc = aica_encode(ko)
    smpb = bytearray(smpb_template[:SMPB_DATA])
    assert smpb[:4] == b"SMPB" and struct.unpack_from("<H", smpb, SMPB_SPLIT + 2)[0] == SMPB_DATA
    struct.pack_into("<H", smpb, SMPB_SPLIT + 6, len(ko) + 1)
    smpb += enc + bytes(-len(enc) % 4) + b"ENDB"
    struct.pack_into("<I", smpb, 8, len(smpb))
    smpb += bytes(-len(smpb) % BLOCK)
    assert len(smpb) <= SLOT_SMPB, len(smpb)
    return bytes(sosb), bytes(smpb), info, f


def voice_records(sec):
    """the PSP's voice records (section 0xD) with its type 4 ones as type 0"""
    recs = [list(struct.unpack_from("<8H", sec, 16 * k)) for k in range(len(sec) // 16)]
    used = {r[1] for r in recs if r[0] >> 8 == 0}
    plays = any(r[0] >> 8 == 4 and r[0] & 0xFF != 2 for r in recs)
    free = min({0, 1, 2} - used) if plays else None
    out = bytearray(sec)
    for k, r in enumerate(recs):
        if r[0] == 0xFFFF or r[0] >> 8 != 4:
            continue
        if free is None:  # only stops, on a slot nothing plays on: none (a next record still follows)
            r[0] = 0xFFFF
        else:
            r[0] &= 0xF0FF
            r[1] = free
            bank, s = r[2] >> 8, r[2] & 0xFF
            if bank:
                r[2] = (bank + 2) << 8 | s
            else:
                r[2] = 0x100 | s if s < 0x15 else 0x200 | (s - 0x15) if s < 0x27 else 0x200 | (s - 0x27)
        struct.pack_into("<8H", out, 16 * k, *r)
    return bytes(out)
