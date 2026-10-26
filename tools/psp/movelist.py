"""The PSP characters' ATTACK LIST in English.

The pause menu's ATTACK LIST draws each character's move list from two
sections of the character's archive: 0x10000E00, a 256x256 texture of 16x16
cells, and 0xA, sprite bank 0x1E's parts made of those cells (the same part
format as the other banks: 12-byte records, then 3-byte tile entries: cell
number, attribute; 0xFF for none). Parts 0-5 and 20 are the frame's
pieces, 6-17 the LEFT / RIGHT markers, 18-19 the name banner, 21-23 the
three tabs, then a 10x2 part per move: the name and the -ism icons on the
top line, the command (motion icons, buttons, notes) on the bottom one.

The PSP's US release has no move list screen; its archives still carry the
Japanese version's lists, which name the moves in Japanese and write notes
and button strengths (弱 中 強) in Japanese. This makes English ones from
them, all from the player's own discs:
  - the font: the Dreamcast's English move lists' name lines, cut into
    letters (a white letter with a dark shadow, letters apart by an empty
    column) and labelled from SAMPLES, the names they show;
  - the icons: the PSP list's own command cells, kept where a cell holds a
    colour (a motion, a button); the cells of white text are the Japanese;
  - each move's English lines (LISTS): the name, and the command as text
    with "@" for the PSP line's icon cells in order;
  - the -ism icons from the Dreamcast's (the PSP's say Z, the Japanese
    name, for A) and the tabs from Ryu's.

    python3 tools/psp/movelist.py <Dreamcast files dir> <out.png>   (the font, to look at)
"""
import os
import struct
import sys

import pac

TW = [sum(((v >> b) & 1) << (2 * b) for b in range(8)) for v in range(256)]
CELL = 16
ROW = 10  # cells a line

# The Dreamcast's move names, in the order of each character's 10x2 parts
# (None: a name in the small font)
SAMPLES = {
    0x00: ["Hadoken", "Shoryuken", "Tatsumaki Senpukyaku", "Shakunetsu Hadoken", "Sakotsu Wari", "Senpukyaku",
           "Fake Hadoken", "Seichu Nidan Tsuki", "Shinku Hadoken", None, "Metsu Shoryuken"],
    0x16: ["Spiral Arrow", "Cannon Spike", "Spin Knuckle", "Hooligan Combination", "Cannon Strike", "Cannon Revenge",
           "Spin Drive Smasher", "Reverse Shaft Breaker", "Killerbee Assault"],
    0x20: ["Sonic Boom", "Somersault Kick", "Reverse Spin Kick", "Spinning Backfist", "Rolling Sobat", "Knee Bazooka",
           "Knee Bazooka", "Somersault Strike", "Sonic Hurricane"],
    0x09: ["Soul Spark", "Soul Throw", "Soul Reflect", "Soul Spiral", "Sliding", "Soul Piette", "Aura Soul Throw",
           "Aura Soul Spark", "Soul Illusion"],
    0x22: ["Air Slasher", "Machine Gun Uppercut", "Double Rolling Sobat", "Jackknife Maximum", "Knee Shot",
           "Sobat Carnival", "Sunrise Theme", "Climax Beat"],
    0x23: ["Tomahawk Buster", "Mexican Typhoon", "Condor Dive", "Condor Spire", "Body Press", "Raging Typhoon",
           "Canyon Splitter"],
    0x07: ["Hozanto", "Bushin Senpukyaku", "Bushin Izuna Drop", "Izuna no Hiji Otoshi", "Hayagake", "Kage Sukui",
           "Kubikari", "Bushin Gokusaken", "Kubikudaki", "Hiji Otoshi", "Bushin Musourenka", "Bushin Hassoken",
           "Bushin Goraikyaku"],
    0x0F: ["Yoga Fire", "Yoga Flame", "Yoga Blast", "Yoga Teleport", "Yoga Escape", "Yoga Shock", "Yoga Inferno",
           "Yoga Strike", "Yoga Stream"],
    0x1B: ["Criminal Uppercut", "Ruffian Kick", "Bad Stone", "Bad Spray", "Knife Pickup", "Knife Throw", "Fake Stone",
           "Sakeru", "Final Destruction", "Dead End Irony", "Tiger Raid"],
    0x02: ["Gou Hadoken", "Zanku Hadoken", "Shakunetsu Hadoken", "Gou shoryuken", "Tatsumaki Zankukyaku",
           "Ashura Senku", "Hyakkishu", "Zugai Hasatsu", "Senpu Kyaku", "Tenma Kujin Kyaku", "Shungokusatsu",
           "Messatsu Gou Hadou", "Tenma Gou Zanku", "Messatsu Gou Shoryu"],
    0x0C: ["Gadoken", "Koryuken", "Dankukyaku", "Kuchu Dankukyaku", "Zenten Chouhatsu", "Kouten Chouhatsu",
           "Saikyo Defense", "Hissho Buraiken", "Shinku Gadoken", "Koryu Rekka", "Chouhatsu Densetsu",
           "Chouhatsu Shinwa"],
    0x10: ["Double Lariat", "Quick Double Lariat", "Banishing Flat", "Screw Pile Driver", "Atomic Suplex",
           "Flying Power Bomb", "Final Atomic Buster", "Aerial Russian Slam"],
    0x18: ["Hundret Hand Slap", "Sumo Head Butt", "Sumo Smash", "Oicho Throw", "Hiza Geri", "Harai Geri", "Oni Musou",
           "Fuji Drop", "Orochi Crush"],
    0x06: ["Jigoku Scrape", "Shiraha Catch", "Butsumetsu Buster", "Daikyo Burning", "Yagura Reverse", "Tengu Walking",
           "Meido no Miyage", "Tenchusatsu"],
    0x1C: ["Flying Barcelona Attack", "Backslash", "Short Backslash", "Izuna Drop", "Rolling Crystal Flash",
           "Sky High Claw", "Scarlet Terror", "Kabe Hari Tsuki", "Back ward Kick", "Rolling Izuna Drop",
           "Scarlet Mirage", "Red Impact"],
    0x0E: ["Patriot Circle", "Stinger", "Mekong Delta Attack", "Mekong Delta Air Raid", "Mekong Delta Escape",
           "Fake Rod", "Spike Rod", "High Jump", "Trick Landing", "Take No Prisoner", "Minesweeper", "Steel Rain"],
    0x0A: ["Psycho Shot", "Double Knee Press", "Head Press", "Somersault Skull Diver", "Bison Warp",
           "Knee Press Nightmare", "Psycho Crusher"],
    0x04: ["Kikoken", "Tenshokyaku", "Hyakuretsukyaku", "Senenshu", "Spinning Bird Kick", "Kaku Kyaku Raku",
           "Yosou Kyaku", "Kohou Kaiten Kyaku", "Senretsukyaku", "Hazan Tenshokyaku", "Kikoshou"],
}


def parts(sec):
    """[(w, h, x, y, next, [(cell, attr) or None])] of a parts section"""
    n = struct.unpack_from("<I", sec, 0)[0] // 12
    out = []
    for k in range(n):
        o, w, h, x, y, nxt = struct.unpack_from("<IBBhhH", sec, 12 * k)
        p, tl = o, []
        for _ in range(w * h):
            if sec[p] == 0xFF:
                tl.append(None)
                p += 1
            else:
                tl.append((sec[p] << 8 | sec[p + 1], sec[p + 2]))
                p += 3
        out.append([w, h, x, y, nxt, tl])
    return out


def encode(recs):
    head, body = bytearray(), bytearray()
    for w, h, x, y, nxt, tl in recs:
        head += struct.pack("<IBBhhH", 12 * len(recs) + len(body), w, h, x, y, nxt)
        for t in tl:
            body += b"\xff" if t is None else bytes([t[0] >> 8, t[0] & 0xFF, t[1]])
    return bytes(head + body)


def cells(tex):
    """a texture section's 256 cells: tuples of 256 ARGB1555 values"""
    px = struct.unpack_from("<65536H", tex, 32)
    out = []
    for k in range(256):
        cx, cy = CELL * (k % 16), CELL * (k // 16)
        out.append(tuple(px[TW[cy + y] | TW[cx + x] << 1] for y in range(CELL) for x in range(CELL)))
    return out


def texture(header, cell_list):
    """a texture section from its header and up to 256 cells"""
    px = [0] * 65536
    for k, c in enumerate(cell_list):
        cx, cy = CELL * (k % 16), CELL * (k // 16)
        for y in range(CELL):
            for x in range(CELL):
                px[TW[cy + y] | TW[cx + x] << 1] = c[CELL * y + x]
    return header[:32] + struct.pack("<65536H", *px)


def line(cs, tiles):
    """the pixels (rows of ARGB1555, 0 transparent) of a line of cells"""
    w = CELL * len(tiles)
    rows = [[0] * w for _ in range(CELL)]
    for i, t in enumerate(tiles):
        if t is None:
            continue
        c = cs[t[0] & 0xFF]
        for y in range(CELL):
            rows[y][CELL * i:CELL * i + CELL] = c[CELL * y:CELL * y + CELL]
    return rows


def ink(v):
    return bool(v & 0x8000)


def light(v):
    """a letter's pixel (white or its grey edge), not its shadow"""
    return ink(v) and ((v >> 10) & 31) >= 12


def runs(rows, test=light):
    """the [start, end) of each run of columns with a letter's pixels"""
    w = len(rows[0])
    cols = [any(test(r[x]) for r in rows) for x in range(w)]
    out, x = [], 0
    while x < w:
        if cols[x]:
            s = x
            while x < w and cols[x]:
                x += 1
            out.append((s, x))
        else:
            x += 1
    return out


def learn_font(dc_dir):
    """{letter: glyph (rows of ARGB1555, 16 high)}, the space's width, the gap between letters"""
    votes = {}
    gaps = []
    spaces = []
    for c, names in SAMPLES.items():
        s = pac.sections(open(os.path.join(dc_dir, f"PL{c:02X}.PAC"), "rb").read())
        cs = cells(s[0x10000E00])
        moves = [p for p in parts(s[0xA]) if p[0] == ROW and p[1] == 2]
        for p, name in zip(moves, names):
            if name is None:
                continue
            top = p[5][:8]
            rows = line(cs, top)
            rs = runs(rows)
            letters = name.replace(" ", "")
            if len(rs) != len(letters):
                continue
            k = 0
            for word in name.split(" "):
                for i, ch in enumerate(word):
                    s0, s1 = rs[k]
                    glyph = tuple(tuple(r[s0:s1 + 1]) for r in rows)  # the shadow column too
                    votes.setdefault(ch, {}).setdefault(glyph, 0)
                    votes[ch][glyph] += 1
                    if i:
                        gaps.append(s0 - rs[k - 1][1])
                    elif k:
                        spaces.append(s0 - rs[k - 1][1])
                    k += 1
    font = {ch: [list(r) for r in max(g.items(), key=lambda kv: kv[1])[0]] for ch, g in votes.items()}
    gap = sorted(gaps)[len(gaps) // 2] if gaps else 1
    space = sorted(spaces)[len(spaces) // 2] if spaces else 5
    return font, space, gap


WHITE, SHADOW = 0xFBDE, 0x8842
# symbols the Dreamcast's names don't have, drawn in the same style: the letter's pixels (#) on
# rows 0-15, a shadow added to the right and below
SYMBOLS = {
    "+": ["", "", "", "", "", "", "..#..", "..#..", "#####", "..#..", "..#.."],
    "-": ["", "", "", "", "", "", "", "", "####"],
    ":": ["", "", "", "", "", "#", "#", "", "", "", "#", "#"],
    ",": ["", "", "", "", "", "", "", "", "", "", "", "#", "#", "#"],
    "/": ["", "", "....#", "....#", "...#", "...#", "...#", "..#", "..#", "..#", ".#", ".#", "#"],
    "(": ["", "", ".#", "#", "#", "#", "#", "#", "#", "#", "#", "#", ".#"],
    ")": ["", "", "#", ".#", ".#", ".#", ".#", ".#", ".#", ".#", ".#", ".#", "#"],
}


def symbol(rows):
    w = max(len(r) for r in rows) + 1
    core = [[x < len(r) and r[x] == "#" for x in range(w)] for r in rows + [""] * (CELL - len(rows))]
    out = [[0] * w for _ in range(CELL)]
    for y in range(CELL):
        for x in range(w):
            if core[y][x]:
                out[y][x] = WHITE
            elif (x and core[y][x - 1]) or (y and core[y - 1][x]) or (x and y and core[y - 1][x - 1]):
                out[y][x] = SHADOW
    return out


class Font:
    def __init__(self, dc_dir):
        self.glyphs, space, gap = learn_font(dc_dir)
        for ch, rows in SYMBOLS.items():
            self.glyphs.setdefault(ch, symbol(rows))
        self.space = space - gap  # blank columns for a space, between two letters' gaps
        self.gap = gap - 1        # blank columns between letters (a glyph ends with its shadow column)

    def render(self, text, gap=None):
        """rows of pixels (16 high) of text"""
        gap = self.gap if gap is None else gap
        rows = [[] for _ in range(CELL)]
        for i, ch in enumerate(text):
            if ch == " ":
                for r in rows:
                    r += [0] * self.space
                continue
            g = self.glyphs[ch]
            if i and text[i - 1] != " ":
                for r in rows:
                    r += [0] * gap
            for y in range(CELL):
                rows[y] += g[y]
        return rows

    def fit(self, text, width):
        """text's pixels at most width wide: closer letters, or squeezed"""
        rows = self.render(text)
        if len(rows[0]) > width:
            rows = self.render(text, 0)
        if len(rows[0]) > width:
            w = len(rows[0])
            rows = [[r[x * w // width] for x in range(width)] for r in rows]
        return rows


def to_cells(rows, n=None, left=None):
    """pixels as cells (left aligned from x = left, or centred in n cells)"""
    w = len(rows[0])
    n = n or (w + (1 if left is None else left) + CELL - 1) // CELL
    x0 = left if left is not None else (n * CELL - w) // 2
    out = []
    for k in range(n):
        c = []
        for y in range(CELL):
            for x in range(CELL):
                sx = k * CELL + x - x0
                c.append(rows[y][sx] if 0 <= sx < w else 0)
        out.append(tuple(c))
    return out


def colours(cell):
    """the -ism letters in an icon cell: X red, A green (the PSP's Z), V blue, L yellow (LV3)"""
    n = {"X": 0, "A": 0, "V": 0, "L": 0}
    for v in cell:
        if not ink(v):
            continue
        r, g, b = (v >> 10) & 31, (v >> 5) & 31, v & 31
        if r > 20 and g > 20 and b < 10:
            n["L"] += 1
        elif r > 20 and g < 12 and b < 12:
            n["X"] += 1
        elif g > 16 and r < 12 and b < 12:
            n["A"] += 1
        elif b > 20 and r < 12 and g < 16:
            n["V"] += 1
    return "".join(k for k in "XAVL" if n[k] > 6)


def isms(dc_dir):
    """{(letters per cell, ...): [(cell pixels, attr), ...]}: the Dreamcast's -ism icon cells, by what they show"""
    lib = {}
    for c in range(0x26):
        path = os.path.join(dc_dir, f"PL{c:02X}.PAC")
        if not os.path.exists(path):
            continue
        s = pac.sections(open(path, "rb").read())
        cs = cells(s[0x10000E00])
        for p in parts(s[0xA]):
            if p[0] != ROW or p[1] != 2:
                continue
            top = p[5][:ROW]
            for n in (2, 3, 4):
                tail = top[ROW - n:]
                if any(t is None for t in tail):
                    continue
                key = tuple(colours(cs[t[0] & 0xFF]) for t in tail)
                if all(key) or (n > 2 and all(key[1:])):
                    clean = top[ROW - n - 1] is None  # (else a name may run into the icons' cells)
                    if key not in lib or (clean and not lib[key][0]):
                        lib[key] = (clean, [(cs[t[0] & 0xFF], t[1]) for t in tail])
    return {k: v for k, (_, v) in lib.items()}


# Each move (the PSP's order): its name, and its command: text, with @ for the PSP line's next icon
# cell (motions, buttons) in order. The PSP's notes in English: 空中で "In air:", 空中壁際で "Air, wall:",
# (タメ加) "(hold)", (強弱で技変化) "(L/M/H vary)", (空中可) "(air also)", 垂直or前ジャンプ
# "Jump up/fwd:", (PPでフェイント) "(PP feint)", 弱 中 強 L M H; ジャンプ "Jump".
LISTS = {
    0x26: [  # Eagle
        ("Manchester Black", "@ + @"),
        ("Canterbury Blue", "@ + @ (hold)"),
        ("Oxford Red", "@ + @"),
        ("Liverpool White", "@ + @"),
        ("Saint Andrews Green", "@ + @"),
        ("Manchester Gold", "@@ + @"),
        ("Union Jack Platinum", "@@ + @"),
    ],
    0x27: [  # Maki
        ("Genko", "@ + @"),
        ("Hayagake", "@@@@ (L/M/H vary)"),
        ("Saka Hayagake", "@@@@ (L/M/H vary)"),
        ("Hassoukyaku", "Air, wall: @ + @ or @"),
        ("Tengu Daoshi", "In air: @ + @"),
        ("Reppukyaku", "@@@"),
        ("Bushin Gokusaken", "L@M@H@H@"),
        ("Bushin-ryuu Seoi Nage", "L@M@H@@+H@"),
        ("Hiji Otoshi", "In air: @ +M@"),
        ("Bushin Gouraiha", "@@ + @"),
        ("Tesshinhou", "@@ + @ + @"),
        ("Ajaratengu", "@@ + @ (air also)"),
    ],
    0x28: [  # Yun
        ("Zesshou-hohou", "@ + @"),
        ("Tetsuzankou", "@ + @"),
        ("Senkyutai", "@ + @"),
        ("Zenpou-tenshin", "@ + @"),
        ("Kobokushi", "@ + @ (@@ feint)"),
        ("Senpuu Kyaku", "@ +M@"),
        ("Kuu Rensui", "Jump L@@ +H@"),
        ("Hakuryu Rengeki", "L@L@M@"),
        ("Hakuryu Tsuuhaida", "M@H@@ +H@"),
        ("Raigeki Shuu", "Jump up/fwd: @ + @"),
        ("Sourai-Rengeki", "@@ + @"),
        ("You-hou", "@@ + @"),
        ("Hiten Souryujin", "In air: @@ + @"),
    ],
    0x29: [  # Ingrid
        ("Sun Upper", "@ + @"),
        ("Sun Lower", "@ + @"),
        ("Sunshot", "@ + @"),
        ("Sunrise", "@ + @"),
        ("Sun Arch", "@ + @"),
        ("Sundive", "In air: @ + @"),
        ("Sunbright", "@ +M@"),
        ("Easy Step", "@ +M@"),
        ("Little Lag", "@ +M@"),
        ("Sunburst", "@@ + @"),
        ("Sunshine", "@@ + @"),
        ("Sun Delta", "@@ + @"),
        ("Lap Roll", "@@@"),
    ],
}
TABS = (21, 22, 23)  # the tabs' parts: Ryu's (English) in place of the PSP's
TEXT_ATTR = 0x20


def icon(cell):
    """a cell with colour in it (a motion or a button), not white text"""
    n = 0
    for v in cell:
        if ink(v):
            r, g, b = (v >> 10) & 31, (v >> 5) & 31, v & 31
            n += max(r, g, b) - min(r, g, b) > 4
    return n >= 3


class Sheet:
    """a new texture's cells, each once"""
    def __init__(self):
        self.cells, self.index = [], {}

    def add(self, cell, attr=TEXT_ATTR):
        if not any(ink(v) for v in cell):
            return None
        if cell not in self.index:
            self.index[cell] = len(self.cells)
            self.cells.append(cell)
        return (self.index[cell], attr)


_cache = {}


def english(cid, sections, dc_dir):
    """the character's move list (sections 0xA and 0x10000E00, the Dreamcast's form) in English"""
    if dc_dir not in _cache:
        ryu = pac.sections(open(os.path.join(dc_dir, "PL00.PAC"), "rb").read())
        _cache[dc_dir] = (Font(dc_dir), isms(dc_dir), parts(ryu[0xA]), cells(ryu[0x10000E00]))
    font, ism_lib, ryu_parts, ryu_cells = _cache[dc_dir]
    cs = cells(sections[0x10000E00])
    recs = parts(sections[0xA])
    sheet = Sheet()
    moves = iter(LISTS[cid])
    out = []
    for k, (w, h, x, y, nxt, tl) in enumerate(recs):
        if k in TABS:
            w, h, x, y, nxt, rt = ryu_parts[k]
            out.append([w, h, x, y, nxt, [t and sheet.add(ryu_cells[t[0] & 0xFF], t[1]) for t in rt]])
            continue
        if w != ROW or h != 2:
            out.append([w, h, x, y, nxt, [t and sheet.add(cs[t[0] & 0xFF], t[1]) for t in tl]])
            continue
        name, command = next(moves)
        top, bottom = tl[:ROW], tl[ROW:]
        # the -ism icons: the Dreamcast's cells showing the same
        n = ROW - max(i for i in range(ROW) if top[i] is None or not icon(cs[top[i][0] & 0xFF])) - 1
        key = tuple(colours(cs[t[0] & 0xFF]) for t in top[ROW - n:])
        ism = ism_lib.get(key) or ism_lib[key[1:]]
        name_cells = to_cells(font.fit(name, CELL * (ROW - len(ism)) - 2), left=1)
        line1 = [sheet.add(c) for c in name_cells]
        line1 += [None] * (ROW - len(ism) - len(line1)) + [sheet.add(c, a) for c, a in ism]
        # the command: the PSP's icon cells in order, English text between them
        icons = [t for t in bottom if t is not None and icon(cs[t[0] & 0xFF])]
        line2, pieces = [], command.split("@")
        if len(pieces) - 1 != len(icons):
            raise ValueError(f"{cid:02X} {name}: {len(pieces) - 1} icons in {command!r}, the PSP line has {len(icons)}")
        for i, text in enumerate(pieces):
            if text.strip():
                rows = font.render(text.strip())
                line2 += [sheet.add(c) for c in to_cells(rows, left=1 if not line2 else None)]
            if i < len(icons):
                t = icons[i]
                line2.append(sheet.add(cs[t[0] & 0xFF], t[1]))
        if len(line1) > ROW or len(line2) > ROW:
            raise ValueError(f"{cid:02X} {name}: the lines take {len(line1)} and {len(line2)} cells")
        out.append([w, h, x, y, nxt, line1 + line2 + [None] * (ROW - len(line2))])
    if len(sheet.cells) > 256:
        raise ValueError(f"{cid:02X}: {len(sheet.cells)} cells, the texture has 256")
    return encode(out), texture(sections[0x10000E00], sheet.cells)


def write_png(path, rows):
    import zlib

    def rgb(v):
        if not ink(v):
            return (40, 0, 60)
        return (((v >> 10) & 31) * 8, ((v >> 5) & 31) * 8, (v & 31) * 8)
    w = len(rows[0])
    raw = b"".join(b"\0" + bytes(c for v in r for c in rgb(v)) for r in rows)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, len(rows), 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    font, space, gap = learn_font(sys.argv[1])
    print("letters:", "".join(sorted(font)), "space", space, "gap", gap)
    rows = [[] for _ in range(CELL)]
    for ch in sorted(font):
        for y in range(CELL):
            rows[y] += font[ch][y] + [0] * 3
    write_png(sys.argv[2], rows)
