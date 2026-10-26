# PSP characters add-on: reference

The add-on puts the four characters only *Street Fighter Alpha 3 MAX* (PSP,
USA) has into the Dreamcast game: Eagle, Maki, Yun and Ingrid. It needs the
player's own PSP disc image. The game stays the Dreamcast's: the new
characters' move code is recompiled from the PSP's MIPS to C and runs inside
it, and their graphics and data are converted to the Dreamcast's formats.
Nothing from either disc goes into the repository or the downloads; the
tools build everything on the player's machine.

The Dreamcast side of everything here is in [dreamcast.md](dreamcast.md).
PSP addresses are the program's own (`BOOT.BIN` loads at 0); Dreamcast ones
are `8C......`.

Status (2026-10-25):

- All four load as their own character numbers (`0x26`-`0x29`) and fight:
  intro, normals, specials and supers register for each
  (build/test/char_test.sh), in every round (see Function map fixes), with
  their own voices (Sound, below).
- Arcade mode: the select screen has the PSP's layout (Yun and Ingrid at the
  ends of the fifth row, Eagle and Maki in the last); picking one goes
  through the mode, -ism and speed screens, the world-map prologue (their
  own portrait, personal data and story: `OPnn.PAC`), the route (the PSP's,
  `arcade.py`), the VS screen (their portraits and names), the fight (the
  HUD's face and name), the win screen (their portrait and quotes:
  `WINnn.PAC`), the wipe and the next stage. Checked through the second
  fight with each of the four (build/test/arc_full.sh).
- The CPU plays them (The CPU, below): checked as player 2 in arcade
  against Ryu for each (build/test/cpu_fight.sh).
- Versus mode's select screen has the PSP's seventh row (Versus mode's
  select screen, below); Training, Team Battle, Survival, Dramatic Battle
  and VS Dramatic use the same screen.
- World Tour: picking one goes through the map, the VS screen and the fight
  (World Tour, below). Checked with Yun to the first fight.
- Endings: each new character's plays to the end (Endings, below; the
  four checked with M.Bison as the last opponent), with the portraits that
  slide through scenes and the closing screen's logo.
- The ranking (The ranking, below): Score Ranking's lists show the new
  characters' faces (checked with the four poked into the lists); their
  ranking portraits (`RK26`-`RK29`, `RK_COM.PAC`) are built the
  Dreamcast's way and checked offline (the screen that shows them wasn't
  reached in testing).
- Not yet: the PSP-only command checker Yun uses for command 0x20
  (recompiled; it takes the PSP's default button layout, except in network
  play, whose per-player state is PSP-only and zeroed); saved data naming a
  new character read by a build without the add-on.
- The three background layers (PSP `0x80F8D0`, `0x80F970`, `0x80FA10`,
  `0xA0` apart; the Dreamcast's `8C188818`, `8C1888B4`, `8C188950`, `0x9C`
  apart) are mapped to `+0x20` (manual maps): the routines that set all
  three (ending objects' and Ingrid's module's: `0x1C966C`, `0x1CA39C`,
  `0x1CAB80`) wrote PSP-only copies of the first and third, and the third's
  X went to the first's (a voted map).

## The PSP program (`BOOT.BIN`)

An ELF without section headers the tools need; the offsets are fixed:

| | Address | File offset | Size |
|---|---|---|---|
| `.text` | 0 | `0x74` | to `0x1F8354` |
| `.rodata` | `0x1F8FF0` | +`0x74` | |
| `.data` | `0x1FEDE0` | +`0x74` | `0xD6138` |
| `.bss` | `0x2D4FC0` | | |
| `.rel.text` | | `0x2D5C18` | `0x7B9F0` |
| `.rel.data` | | `0x353CD0` | `0x31E00` |

Address = file offset - `0x74`. The relocations (8 bytes: offset, type) are
what tells code addresses from numbers: `.rel.text` has types 4
(`R_MIPS_26`, calls), 5 (`HI16`) and 6 (`LO16`), `.rel.data` type 2
(`R_MIPS_32`, a pointer in data).

It is a PRX (ELF type `0xFFA0`) with two segments: 0 at address 0 (`.text`
to `.data`, `0x2D4F30` bytes) and 1 at `0x2D4F30` (64 bytes of data, then
the `.bss`, `0xA4D4E4` bytes in all). A relocation's info is type (byte 0),
offset segment (byte 1) and address segment (byte 2): an address built for
segment 1 is relative to `0x2D4F30`. The tools (and the tables here) use the
raw numbers, which is consistent for `.bss` addresses but means a segment-1
number below `0x2D4FC0` reads as a segment-0 one: 1003 instructions build
such numbers (the random number generator's state, raw `0x28634`, is really
`0x2FD564`). Only one is in code the add-on recompiles: `0x1DC6D4` in
`0x1DC324` (raw `0x33404`, really `0x308334`).

## PSP memory

| PSP | Dreamcast | What |
|---|---|---|
| `0x80AF70` | `8C17FD38` | fighters, 4 x `0x498` (Dreamcast `0x44C`) |
| `0xA37080` | `8C180E68` | projectiles, 24 x `0x168` (`0x148`) |
| `0xA39840` | `8C1832A8` | effect pool A, 32 x `0xC0` (`0xB0`) |
| `0xA3B040` | `8C1848A8` | effect pool B, 16 |
| `0xA3BC40` | `8C1853A8` | effect pool C, 16 |
| `0xA3C840` | `8C185EA8` | effect pool D, 42 on the PSP, 16 on the Dreamcast |
| `0x39C9E0` | `8C17FBC0` | game state (delta `0x8BDE31E0`) |
| `0x8082F8` | `8C17FC60` | second game state block |
| `0x80F970` | `8C1888B4` | camera block (`tools/psp/manual_maps.json`) |
| `0x213F88` | | fighter pointer table |
| `0x2C7F00`-`0x2C8500` | | effect palettes (ABGR) the characters share |

### Fighter layout

The PSP inserted fields; Dreamcast offset -> PSP offset:

| Dreamcast | PSP |
|---|---|
| `0x000`-`0x037` | same |
| `0x038`-`0x12F` | +`0x10` |
| `0x130`-`0x32B` | +`0x20` |
| `0x32C`-`0x44B` | +`0x24` |

PSP-only bytes: `0x38`-`0x47`, `0x140`-`0x14F`, `0x34C`-`0x34F`,
`0x470`-`0x497`. So the descriptor pointer at Dreamcast +0x180 is PSP
+0x1A0, the character number +0x132 is +0x152, the command blocks at +0x32C
are at +0x350.

Effect objects: `0x10` bytes inserted at `0x38`. Projectiles: laid out like
the first `0x148` bytes of a fighter (`0x168` on the PSP).

## The PSP's characters

41 descriptors from `0x23F8FC` (Ryu), in the Dreamcast's character order
without `0x13`; the add-on's four are last:

| | Number | Descriptor | Entry | Code | Own data | Index table |
|---|---|---|---|---|---|---|
| Ryu | `0x00` | `0x23F8FC` | `0xE971C` | `0xE971C`-`0xEE210` | `0x23B858`-`0x23FD28` | `0x23F998` |
| Eagle | `0x26` | `0x2A3198` | `0x1B3660` | `0x1B0230`-`0x1B422C` | `0x29FE60`-`0x2A3340` | `0x2A3234` |
| Maki | `0x27` | `0x2A6268` | `0x1B9E60` | `0x1B422C`-`0x1BAC0C` | `0x2A3340`-`0x2A63C8` | `0x2A6304` |
| Yun | `0x28` | `0x2AD6DC` | `0x1BF3E4` | `0x1BAC0C`-`0x1C1DF8` | `0x2A63C8`-`0x2AD9D0` | (code: `0x1BF93C`) |
| Ingrid | `0x29` | `0x2B1EE0` | `0x1C6194` | `0x1C1DF8`-about `0x1CB0A4` | `0x2AD9D0`-`0x2B22EC` | `0x2B1F7C` |

The four also use a helper library at `0xCE000`-`0xD1000` that no Dreamcast
character has: `0xCEE04` plays a voice (sound number `| 0x200`, through
`0x78408`), `0xCFEC0` wraps the animation step.

### Entry and the descriptor block

The engine calls a character's entry with the fighter. It copies the
descriptor to `*(fighter + 0x1A0)` (the "D block", `0xA08` bytes; the 39
descriptor words are its start) and calls the builder `0x6AB48(fighter, 0,
index table)`, which fills in words 0-11 and 32-34 from the archive's section
0x11 (the Dreamcast has them in the character file instead). The builder
calls `0x6A638(dst, D[0xC4], D[0xC0], index)` for each index byte; D[0xC0]
and D[0xC4] point at section 0x11 and its data (+0x208).

The index bytes are taken in this order (byte -> D offset written):

| Bytes | D offsets |
|---|---|
| 0-3 | `0x0C8`, `0x308`, `0x548`, `0x788` (words 0-3 point at them) |
| 4-10 | `0x9D4`, `0x9D8`, ..., `0x9EC` |
| 11, 12 | `0x9F0` (word 7, D+0x1C points at it), `0x9FC` (D+0x24) |
| 13-17 | `0x10`, `0x18`, `0x20`, `0x28`, `0x2C` |
| 18-20 | `0x80`, `0x84`, `0x88` (words 32-34: palettes) |

Then D[`0x9C8`..`0x9D0`] = &D[`0x9D4`] and D+0x14 = &D[`0x9C8`].

Yun differs: his entry calls his own builder `0x1BF93C(fighter, 0x28)` and
`0x1BFB0C(fighter, 4, 0)`, and his words 0-3 point straight at his data. The
build runs entries in an interpreter (below), so either way works.

### Archive section 0x11 (animation data)

u32 0, u32 data size, 64 u32 offsets at +8, 64 s32 counts at +0x108; the
data starts at +0x208 ("A"). What `0x6A638` writes for entry *i*:

| Count | Written |
|---|---|
| 0 | one pointer: A + offset |
| -1 | a list: A + 10 x v for each s32 v at A + offset until -1 |
| > 0 | count pointers: A + offset[i + v + 1] for each v at A + offset |
| < -1 | nothing |

Ten bytes is the animation record (dreamcast.md, Animation record).

### Other archive differences

| | PSP | Dreamcast |
|---|---|---|
| tiles (0x200000) | pixels row by row | twiddled |
| palettes (0x400000), words 32-34 | ABGR1555 | ARGB1555 |
| sound | `0x40000002` PPHD, `0x40008002` VAG | `0x40000002` SOSB, `0x20000005` SMPB |
| `0x10000E00` | PVR texture, PSP pixel order | PVR texture, twiddled |
| word 38 entry 1 (dialogue) | 0 | dialogue table |

### Sizes

Each player slot has `0x140000` bytes for a character's sprites. Ryu fits,
Eagle fits with `pac.lz_opt` (the tiles recompressed optimally, within 0.3%
of Capcom's own), Maki fits (`0x119BFF`); Yun (`0x14578C`) and Ingrid
(`0x1661DB`) don't, so their tiles go to the extension RAM (Runtime, below).

## Engine functions: PSP -> Dreamcast

Found by `tools/psp/align.py` and checked by hand:

| PSP | Dreamcast | What |
|---|---|---|
| `0x1ED2C` | `8C02554E` | projectile allocator |
| `0x1EEAC` | `8C02564C` | effect pool A allocator |
| `0x1EF54` | `8C0256B8` | effect pool D allocator |
| `0x1EFE4` | `8C02570A` | generic effect allocator |
| `0x1D330` | `8C024698` | |
| `0x1D368` | `8C0246A4` | |
| `0x5AEFC` | `8C04B378` | descriptor word 31 |
| `0x5D63C` | `8C04C15A` | |
| `0x5D674` | `8C04C16E` | per-ism table lookup (word 7, fighter +0x162) |
| `0x5D6EC` | `8C04C1AC` | |
| `0x5D848` | `8C04C29C` | animation step |
| `0x63708` | `8C04EBDC` | command check |
| `0x661E4` | `8C04F9BA` | |
| `0x67F84` | `8C050AEC` | |
| `0x78408` | `8C059F2C` | play a sound |

`build/test/maps.json` (from `align.py --maps`) has the full lists, functions
and global segments.

## Building a character (`tools/psp/`)

| Tool | What |
|---|---|
| `mips.py` | Allegrex decoder |
| `mipsgen.py` | MIPS -> C: `Boot` (program, relocations), `Function` (control flow, jump tables), `Gen` (C) |
| `align.py` | pairs PSP and Dreamcast functions by what they do: loads and stores with their offsets, globals, calls, calls through tables; `--maps` writes maps.json |
| `resolve_globals.py`, `resolve_data.py` | PSP globals and data -> Dreamcast |
| `build_char.py` | everything else, below |
| `mipsemu.py` | MIPS interpreter: runs a character's entry at build time |
| `pac.py` | archives: sections, tiles (LZ, `lz_opt`, twiddle), palettes |
| `sprites.py` | sprite banks: parts and tiles, adding the PSP's parts to a Dreamcast bank |
| `chartables.py` | the Dreamcast tables indexed by character number that get moved, the new entries, the file names |
| `screens.py` | the select and VS screens: `SELECT.PAC` with the PSP characters' sprites, the tables' entries |
| `files.py` | the characters' files: `PLnn.PAC` (archive), `OPnn.PAC` (prologue), `WINnn.PAC` (win screen), `BDnn.PAC` (pre-fight dialogue's sprites) |
| `arcade.py` | arcade routes: the PSP's way of making them, run for each of the Dreamcast's 16 |
| `throws.py` | every Dreamcast character's throw positions for the new characters as the held opponent |
| `hud.py` | the fight HUD (`PITU.PAC`): the new characters' faces and names |
| `dc_idtables.py` | finds the Dreamcast tables indexed by character number |

`build_char.py <BOOT.BIN> <PSP data> <Dreamcast files> <out> <maps.json>
<char>=<file id>,...` (for example `0x26=0x26`). For each character:

1. Collects its code (entry, own range, the helper library) and data (own
   region, clipped at the next character's; shared ranges at 16-byte
   granularity; the shared palettes whole).
2. Runs the entry in `mipsemu.py` against scratch memory (fighter
   `0x0B000000`, D block `0x0A000000`, section 0x11 `0x0C000000`, stack below
   `0x0F000000`), letting it call only its own code, `0x6AB48` and
   `0x6A638`. What it wrote into the D block becomes the descriptor's tables:
   section 0x11 offsets, D offsets, zeros or PSP pointers.
3. Writes `PL<id>A.BIN`, `B` and `C` (one per overlay slot; the addresses in
   each are that slot's) and `PL<id>.layout.json`.
4. Writes `SELECT.PAC` (`screens.py`), `PITU.PAC` (`hud.py`), `PLnn.PAC`,
   `OPnn.PAC`, `WINnn.PAC` and `BDnn.PAC` (`files.py`); the character file's
   pre-fight dialogue comes from the PSP's `bdnn_u.pac` (section `0x10001`).
5. Writes the recompiled code: `psp_code_<n>.c` and `psp_tables.c` (the
   function table, regions, code map, and `g_psp_char_data`: the patches the
   runtime applies at boot, from `chartables.patches`, `file_patches`,
   `screens.build` and `arcade.patches`), which the runtime build picks up
   from `build/psp_out`.

### Character file (`PLnnA/B/C.BIN`)

| Offset | What |
|---|---|
| `0x000` | entry stub |
| `0x008` | a stub per function the engine may call |
| | PSP data with no Dreamcast counterpart the code uses |
| | the character's own data: descriptor (with the 12-byte `[8, code, -1]` header before it), tables, pointers converted |
| | animation data (section 0x11's, on the Dreamcast it lives here) |

A stub is 8 bytes: `FFFF 5053 <index> FFFF`. The Dreamcast never runs it:
`call_indirect` finds the magic and calls recompiled function *index*.

Conversions while writing: palettes ABGR -> ARGB (also the shared palette
area), the dialogue table (word 38, entry 1) from the Dreamcast character's
own text (`dc_text`), PSP pointers to fighters into Dreamcast ones.

## Runtime

| File | What |
|---|---|
| `runtime/psp.h`, `psp.c` | the recompiled code's support |
| `runtime/psp_load.c` | loading: big tile sections, `SELECT.PAC`'s bigger sections, the new characters' files, the select screen's portraits' palettes and extra cells, the use counters |
| `runtime/chartables.c` | the moved tables: copied at boot and reset, then the add-on's patches |
| `runtime/psp_none.c` | empty tables when no PSP character is built |
| `runtime/sprite_log.c` | `SFA3_SPRITE_LOG` (debug) |
| `runtime/gdi.c` | extra files on the disc |
| `runtime/mmio.c` | the extension RAM |

### Recompiled code

Registers hold Dreamcast addresses. MIPS state is `MipsCtx m` (`$sp` is the
SH-4's r15). Two conversions:

- `psp_add(p, offset)`: when p is in the fighter, projectile or effect
  arrays (`8C17FD38`-`8C1869A8`), the offset is a PSP one and is converted
  with the layout maps above, crossing element boundaries if the code
  indexes an array that way. PSP-only fields go to side storage: addresses
  `F0000000 | array << 20 | index << 12 | PSP offset`.
- `psp_xa(a)`: an address the code builds from a `HI16`/`LO16` pair. Looked
  up in the regions (linear, the player slot's file, fighters, pools) and the
  code map; logs "no Dreamcast counterpart" when there's none.

Calls to Dreamcast functions (`psp_call_dc`) pass MIPS a0-a3 as r4-r7 and
t0-t3 on the SH-4 stack. The player slot of the file a stub was called in
picks whose data the constants mean.

### Extension RAM

16 MB at physical `0x18000000` (SH-4 area 6, guest `0x98000000`; nothing
there on a real Dreamcast), served by `mmio.c` from `g_ext_ram`, cleared at
reset:

| Address | What |
|---|---|
| `98000000` + slot x `0x200000` | sprite tile sections too big for a player slot (`hle_pac_section`, wrapping `f_8C01348E`: the section goes here, with `0x800` bytes kept for the palettes, and the per-slot and per-bank tile tables point here) |
| `98600000` | `SELECT.PAC`'s sections bigger than their places (bank 8 tiles `+0`, palettes `+0x200000`, parts `+0x220000`, bank B tiles `+0x260000`, parts `+0x2E0000`) |
| `98900000` | `PITU.PAC`'s bank 9 parts (`hud.py`: the loader's destination table patched) |
| `98910000` | `PITU.PAC`'s bank `0x10` tiles |
| `98C00000` + fighter x `0x100` | a fighter's 42-entry copy of its throw table (`psp_load.c`, `throw_table`) |
| `98D00000` | the moved tables (`chartables.py`, `EXT`) |
| `98E00000` | records the new table entries point at (`chartables.patches`) |
| `98E20000` | throw positions: u32 [character file][4] list addresses, the lists (`throws.py`) |
| `98E30000` | the add-on's file names (`chartables.file_patches`) |
| `98E31000` | arcade routes (`arcade.py`) |
| `98E40000` | the select and VS screens' records (`screens.py`) |
| `98E80000` | PSP-only variables the recompiled code uses (`build_char.py`), to `98F00000` |

### New character numbers

- Files: the game opens files by their number in the name table, and a
  character's by family (pl, op, rk: base + character, which runs into the
  next family for `0x26` and up). The name table is moved with room for the
  add-on's names; the openers that number by character are wrapped to use
  them (Files per character, below).
- Overlay: the template already makes `PL26A.BIN` etc.
- Disc: the files aren't on the disc. `SFA3_ADD=NAME=path;...` adds files:
  `gdi.c` serves a rebuilt root directory at LBA `0x7F0000` and the files'
  data from LBA `0x800000` up. Everything that says where the root is gets
  patched as it's read: the volume descriptor, both path tables (the game's
  file system finds the root through them; missing this gave DISC LOAD
  ERROR) and the subdirectories' `..` records.
- Tables indexed by character number: moved, see Character tables.
- The use counters (`8C09B20C`, 8 bytes a character: a u16 per -ism, part of
  the system data saved to the memory card, the ranking right after it):
  `f_8C069DCC` counts a game; wrapped (`hle_count_use`) to skip the PSP
  characters, who have no room there.

### Character tables

The Dreamcast's tables indexed by character number have 38 entries (some
40), packed one after another. `tools/psp/chartables.py` lists them; the
recompiler moves every listed table (literals pointing into it rewritten) to
the extension RAM with room for 42 entries, and writes the list
(`char_tables.c`). At boot and reset `runtime/chartables.c` copies the
Dreamcast's entries and fills the new ones with copies of entry 0 (Ryu's);
with the add-on built, `build_char.py` adds the PSP's entries
(`g_psp_char_data` in `psp_tables.c`), checked against the Dreamcast's.
The PSP code's own references to these tables go to the moved copies, which
have the PSP's 42-entry layout.

Added in 0.2.2: the ATTACK LIST's rows (`8C0D5064`, kind `ptr_rank`), the
pre-fight dialogue choices (`8C0A8468`, and `8C0A8490` / `8C0A8508` a row
each, 44 bytes apart, with `chartables.REPLACE` changing the code's row
stride), the dialogue faces (`8C0C0E04`), and the arcade select's character
cells (`8C0BE53C`: (column, row)
per character, where the cursor starts, e.g. after a continue; the PSP's
`0x2230B7`; without it Yun's cursor started on Maki's cell). The versus
select's twin (`8C0C2140`) was moved already.

Two tables by object type are moved the same way (with no entries added:
the characters register their handlers as a fight starts, `f_8C046D6E`).
The new characters' types run past the Dreamcast's ends: written there, a
handler landed in the next table, the pool C one, whose types 2 and 3 are
stage objects; on the next stage with such objects (the USA) Ingrid's code
ran for them and wrote her pointers over another object, which then called
garbage. Word 29's overflow went into word 28's table.

| Dreamcast | Entry | PSP | What (where read) |
|---|---|---|---|
| `8C0B2088` | 4 | `0x2154E0` | `f_8C04BF86` |
| `8C0B21BC` | 4 | `0x215632` | one byte per -ism -> fighter +0x231 (`f_8C04EB60`, literal +1) |
| `8C0B2254` | 4 | `0x2156DA` | `f_8C04EB76` |
| `8C0B57B8`, `8C0B57DE` | 1 | `0x219B88`, `0x219BB2` | `f_8C0546D6` |
| `8C0B7C08`, `8C0B7CC0` | ptr | `0x21C02C`, `0x21C0F4` | `f_8C05090C`, `f_8C050982` (records the Dreamcast has) |
| `8C0B7D58` | 1 | `0x21C19C` | `f_8C04E900` |
| `8C0B7D7E`, `7E16`, `7EAE` | 4 | `0x21C1C6`, `0x21C26E`, `0x21C316` | -ism 0, >0, <0: fighter +0x2A0 points at one (`f_8C04E900`) |
| `8C0B7F46`, `8406`, `88C6` | 32 | `0x21C3BE`, `0x21C8FE`, `0x21CE3E` | -ism 0, >0, <0: fighter +0x2A4 |
| `8C0B8DA8`, `8DCE`, `8DF4`, `8E1A` | 1 | `0x21D3A0`... | `f_8C04E900`, `f_8C049A86`, `f_8C049D28` (one is the guard meter: wrong, it showed DANGER) |
| `8C0B9A74` | 1 | `0x21E0D0` | `f_8C056B4C` |
| `8C0BA66C` | 1 | `0x21ED30` | `f_8C05A010` (sound) |
| `8C0BBD44` | 2 | `0x220408` | `f_8C05B05E`; 37 entries (the 38th is the next object, on the PSP too) |
| `8C0BC0BC` | 4 | `0x220798` | `f_8C05C01C` |
| `8C0BC5D8` | 2 | `0x220C80` | `f_8C05D732`; 40 entries |
| `8C0C0E54` | 1 | `0x2260EC` | `f_8C06AA2C`: a character number |
| `8C0C15A0` | 1 | `0x226BC8` | `f_8C06D1C8` |
| `8C0C1B14` | 2 | `0x2270B0` | `f_8C06DC3C` |
| `8C0D5498` | 1 | `0x239348` | `f_8C0904D6` |
| `8C0B4DF8` | 32 | `0x2190C8` | `f_8C055460`: a row per character, indexed by fighter +0x157 (the code's literal is the row base, 16 bytes before the first non-zero byte) |
| `8C09895C` | 4 | - | the file name table: 271 names, room for the add-on's 24 after them |
| `8C09B08E` | 2 | - | win screen file (names[] index; `f_8C01C2A6`) |
| `8C09B0DE` | 1 | - | win screen, a byte per character |
| `8C09B116` | 2 | - | bd file (`f_8C01CA18`) |
| `8C09AC12` | 2 | - | ending file (names[] index, 0 = none; `f_8C01BA82`, `f_8C01BC2A`); 40 entries |
| `8C0A6DF8` | 1 | `0x20B1C4` | `f_8C02B774` |
| `8C0A70AE` | 1 | `0x20B3A8` | `f_8C02C434` (a lookup function) |
| `8C0A70D6` | 1 | `0x20B3D2` | `f_8C02C43A`: a character number (0x13 -> 0x11) |
| `8C0A8248` | ptr | - | arcade routes: 16 x 8 opponents (`arcade.py` makes the new ones) |
| `8C0A7176` | 1 | `0x20A784` | the route's 9th opponent, the rival |
| `8C0A719E` | 1 | `0x20A7AE` | the route's last opponent, the boss |
| `8C0A8380` | 1 | `0x20B64C` | `f_8C02C758` |
| `8C0A83A8`, `83D0`, `83F8` | 1 | `0x20B676`, `0x20B6A0`, `0x20B6CA` | a flag per character each |
| `8C0A8420` | 1 | `0x20B604` | the character's stage (PSP `0x2D798`); `0xFF`: none (Ingrid) |
| `8C0CF7D4` | 1 | `0x234154` | prologue: the portrait's first part in the OP file's bank `0x15` (`f_8C07E584`) |
| `8C0C1350` | 1 | `0x20A7AE` | the boss again (`f_8C06C02C` & co.) |
| `8C0BE434` | 1 | `0x222FF6` | select (`f_8C06920E`) |
| `8C0BE266` | 1 | `0x222D4C` | `f_8C06443C` |
| `8C0BED8C` | ptr | - | animation by kind of object (30): one of two record groups (VS portraits and names, below) |
| `8C0C03B4` | ptr | - | select: big portrait animation (`f_8C06BAD8`) |
| `8C0C1194` | 2 | - | select: name, a bank 8 part (`f_8C06B272`) |
| `8C0AB820`, `8C0C0D26` | 2 | - | the names again (`f_8C036730`, `f_8C06A5A8`) |
| `8C0C11F4` | 4 | - | select: the name strip down the side: u16 spacing (0x8000: the name 8 up), u16 bank 8 part (`f_8C06B488`); the PSP's are two tables, `0x226914` (u16) and `0x2C8ABC` (u8 + `0xF4` = part) |
| `8C0C142C`, `8C0C1478` | 2 | - | select: name banner (bank 8 part) and its width |
| `8C0B39C8` | ptr | `0x2178C8` | command definitions (not by character): the PSP adds 0x40 and 0x41 |
| `8C0A9278` | 4 | | object handlers by type, effect pool D (descriptor word 29): 47 -> 59 entries; the new characters' types 47-58 |
| `8C0A9334` | 4 | | object handlers by type, projectiles (descriptor word 28): 43 -> 47 entries; types 43-46 |

Found three ways: following the character number through the code
(`tools/psp/dc_idtables.py`); a read-trace build comparing two fights
(`tools/read_trace.py`: an instruction that reads `a` with one character and
`a + k x s` with another reads a table of `s`-byte entries); and code reading
(the per-ism tables are reached through fighter +0x2A0/+0x2A4). Then a scan
(build/test/scan_tables.py): every data literal in the recompiled code whose
38 entries the PSP has with 4 more after them (the PSP's next bytes differing
from the Dreamcast's; equal ones mean a table the PSP didn't extend:
`8C0BA00E`, `8C0BBC2C`). And the arcade flow traced with Yun against Dan
(build/test/flow_trace.sh): that a new character reads past a table's end
doesn't matter to the comparison.

Still to look at: `8C0B8F74` (intro pairings: lists of (character, value)
ending `FFFF`; values index `8C0B2154`/`8C0B2162`, which the PSP extended
too), `8C0BD570` (17-byte records), `8C0C111C` (32 entries: `f_8C06AD34`,
`f_8C06ADE4`), `8C0C1AC8` (u16, right before `8C0C1B14`: `f_8C06DC3C`),
`8C0D2354`, `8C0D5064`, `8C1D1608` (RAM), `8C0B1D44`, `8C0AD4E0` (16-byte
records, the code's literals at +4, +0x10, +0x1C, +0x28), and the scan's
other candidates (`8C0A8468`, `8C0AB238`, `8C0BBE8C`, `8C0BC314`,
`8C0D3CA4`).

Not by character: `8C0C19C8` (map positions, (x, y) by stage).

### Files per character

The file name table (`8C09895C`, 271 names) has families by character:

| Family | Names | Opened by |
|---|---|---|
| `pl` (archive) | 13-50 | `f_8C01B598`: names[13 + character] with `f_8C012C80` (return `8C01B648`) |
| `st` (stages) | 51-88 | by stage (`8C09ABE8`, 42 bytes: the Dreamcast has stages `0x26`-`0x29` too) |
| `rk` (ranking portraits) | 90-125 | `f_8C01C8D0`: names[90 + position] with `f_8C012C80` (return `8C01C9F6`) |
| `op` (prologue) | 126-161 | `f_8C01B904`: `f_8C0126F4(126 + position)` (return `8C01BA44`) |
| `win` | 162-194, 267-270 | table `8C09B08E` |
| `bd` (win portraits) | 195-227 | table `8C09B116` |
| `end` | 228-260 | table `8C09AC12` |

The position is the character number, but `0x13` -> `0x11`, `0x25` -> `0x15`,
and one less above `0x12` (the second Gen and Balrog have no files of their
own). For `0x26` and up, base + position is the next family's file.

The add-on's names follow the Dreamcast's: number 271 + 4 x family +
(character - `0x26`), families in the order pl op rk win bd end
(`chartables.FAMILIES`, `psp_load.c`); the names themselves at `98E30000`.
`hle_open_file` (wrapping `f_8C012C80`) swaps in the add-on's name for the
pl and rk callers, `hle_open_numbered` (wrapping `f_8C0126F4`) the number
for the op caller, the tables get the new numbers; only for families the
add-on has built (a spare entry is a copy of names[0]).

The PSP has all of them for the new characters (`bd26_u.pac`,
`end26_u.pac`, `op26_u.pac`, `rk26.pac`, `win26_u.pac`, and `pl29r_u.pac`).
Built so far (`files.py`):

- `WINnn.PAC`: the win portrait (sections `0x1` parts, `0x2` tiles, `0x3`
  palettes; the same bytes as the PSP's but the tiles twiddled and the
  palettes swapped), the win quotes (`0x0`: lines one after another, a
  missing second line left out; the PSP's `0x10002` has a 128-byte block a
  quote: 36 of the 37 characters' come out byte for byte, the other has a
  PSP fix), and the sound (`0x40000000`, `SOSB`; the PSP's is `PPHD` +
  `0x40008000` data): still Ryu's.
- `BDnn.PAC`: the pre-fight dialogue's extra sprites (bank `0x1A`, when there
  are any) and the dialogue in Japanese (a texture, blank). The English
  dialogue is in the character file (word 38, entry 1).

- `PLnn.PAC`: the PSP's sprite parts, tiles (twiddled), palettes (red and
  blue swapped), the voice records (`0xD`, converted) and banks
  (`sound.py`).
  The move list (the pause menu's ATTACK LIST): section `0xA` (bank `0x1E`'s
  parts) and `0x10000E00` (their cells, a 256x256 texture; the PSP's is
  linear, ABGR1555: `pac.convert_texture`). The PSP's US release has no
  move list screen, and its archives keep the Japanese version's lists for
  the four new characters (Japanese names, notes and button strengths; the
  banner English): `movelist.py` makes them English (below).
- `OPnn.PAC`: bank `0x15` (the portrait and the screen's frame; parts 0-3
  and 11-14 are the frame, which the PSP draws differently: its own kept),
  palettes (68, swapped) and after them the text: personal data (10 lines of
  18 bytes; the PSP's section `0x10004`, the same bytes) and the story
  (four blocks of 69 bytes: lines with their position bytes; the PSP's
  section `0x10000` has three blocks of 128). Section `0x10000A00` is the
  story in Japanese (a 256x256 texture): left blank.

A `bd` file: win portrait sprites (sections `0x100001A`, `0x200001A`) and a
texture (`0x10000900`); the PSP's adds the win quotes (`0x10001`, text).

### Arcade mode

- Route (`f_8C02BD08`): 8 opponents from the character's 16 routes
  (`8C0A8248`, chosen by game state +0x21), then the rival (`8C0A7176`) and
  the boss (`8C0A719E`); a pattern (`8C0A82E0`, by game state +0x22)
  swaps in the other Chun-Li (`0x12`) and Sodom (`0x14`). The PSP makes the
  route at the start instead (`0x2ACD0`): a Mersenne twister (`0x1CB7C`
  seeds, `0x1CBE0` gives the low byte of each tempered word) seeded with
  +0x21 + `0x1C3`; place 4 is the character's 5th opponent (`0x20A7D8`),
  8 and 9 the rival and boss; the others the first of the character's
  weighted opponents (`0x20ACC0`: opponent and 10 chances out of 256) whose
  chance comes up and that fits, or else random % 42 until one fits that
  isn't excluded (`0x20AA24`). Fits (`0x2B024`): not in the route already
  (the other Chun-Li and Sodom counting as the same), not from the previous
  opponent's stage (`0x20B604`). `arcade.py` runs that for each route
  number; the new characters' routes have the others as opponents, as on
  the PSP.
- Prologue on the world map: the OP file; the portrait is bank `0x15` from
  part `8C0CF7D4[character]` to the last.
- VS screen: the portraits are bank B parts of `SELECT.PAC` (a chain: the
  part record's byte 10 set means the next part belongs to it), set as an
  object's animation: group + 8 x character, the group from `8C0BED8C`
  (a pointer per kind of object: `8C0BEB3C`, the portraits, bank B, or
  `8C0BEC64`, bank 8). The PSP's groups are `0x2237AC` and `0x2238FC`, 42
  records each.
- VS screen palettes: `f_8C069D34(character, -, colour, slot)` loads 4 bank
  8 palettes from `0x148` + character x 24 + colour x 4 (the PSP the same,
  its new characters' at `0x4D8`-`0x537`); the add-on appends the PSP's 96
  at an index that formula reaches with character + a shift (`hle_vs_palette`).
- Fight HUD: `f_8C06443C` sets the parts every frame: faces bank `0x10`
  (`PITU.PAC`; the PSP's `pit_u.pac`) `0x17D` + character (player 1, at
  HUD +`0x30C`) and `0x1A6` + character (player 2, +`0x94C`), names bank 9
  `0xF6` / `0x121` + `8C0BE266`[character] (+`0x330` / +`0x970`). The PSP:
  faces `0x17D` / `0x1AA` + `0x222D76`[character], names `0xF6` / `0x125` +
  `0x222D4C`[character]. Bank 9's tiles are 16x16 cells of `PITU.PAC`'s three
  256x256 textures (tile n: texture n / 256); the Dreamcast's names are two
  cells high with the text in the lower row, the PSP's one. `hud.py` adds the
  PSP's faces (bank `0x10` `0x466`-`0x46D`) and names (bank 9 `0x162`-`0x169`,
  their cells copied into the third texture's unused ones, from 705), and
  `hle_hud` puts them in.
- Throws: a character's descriptor word 9 (+36) is a table by the opponent's
  character (38 entries) of where the held opponent goes, frame by frame
  (`f_8C05206E`, `f_8C0520F0`, `f_8C052180`, `f_8C052268`). The PSP builds
  42 from section `0x11` (a pool entry; its lists for the old opponents are
  the Dreamcast's byte for byte); `throws.py` takes the other four for every
  Dreamcast character file (Gen's descriptor as `align.Dc` finds it is five
  words late: its word 4). `throw_table` (wrapping the four) gives the
  attacker's descriptor (fighter +`0x180`, the fighter's own copy) a 42-entry
  copy when the opponent is a PSP character. Without it, Ken's CPU code read
  garbage (`512F51xx`) after a KO.

### Engine functions only the new characters call

`build_char.py` recompiles PSP engine functions with no known Dreamcast
counterpart. Confirmed by hand (`tools/psp/manual_maps.json`):

| PSP | Dreamcast | What |
|---|---|---|
| `0x1D044` | `8C024568` | object physics step (position += velocity, velocity += acceleration) |
| `0x1F338`, `0x1F3BC` | `8C025930`, `8C025998` | menu object spawn and free (PSP objects 0x94 bytes, DC 0x90; same fields below 0x90) |
| `0x5DEDC`, `0x5E068` | `8C04C6D8`, `8C04C7F8` | fighter state change |
| `0x5DF68` | `8C04C73C` | the same, with CPU branches (`0x77E00`=`8C059E8C`, `0x5FD4C`=`8C04D974`, `0x5F114`=`8C04D122`); the PSP also sets fighter +0xC0 (DC +0xB0) to 2 around it |
| `0x67B1C` | `8C05082C` | from neutral: start walking or crouching (reads the pause flags `8C17FC6D`/`8C17FBE6` and the lever, calls the checks `0x67CB8`/`0x67E08`/`0x67D68` = `8C05090C`/`8C0509DC`/`8C050982`; returns 1 when it started one) |
| `0x67E50` | `8C0509FE` | its neighbour (calls `8C04D246`, `8C051CFE`) |

### Return values

A Dreamcast function called in a PSP one's place returns its result the
Dreamcast's way: SH-4 byte and halfword loads sign-extend where the PSP's
`lbu`/`andi` don't. The random byte (`f_8C0246A4`, the PSP's `0x1CBE0` via
`0x1D368`) is the one that mattered: the PSP code's `random % n` came out
negative half the time and read before its tables (Ingrid's and Eagle's win
pose choice: Ingrid's then skipped setting her win's countdown, PSP +0x33C,
and the round never ended). `psp_call_dc` returns it as the PSP's, 0-255.
The other calls whose results the PSP code uses return words.

### Switch tables

`.rodata` (`0x1F8FF0`-`0x1FEDE0`) has no relocations, so its jump tables
hold raw code addresses and aren't in the data relocations the recompiler
used to read a table's entries by: six switches in the new characters' code
(tables `0x1FD7B0`-`0x1FDD20`, Eagle's and Maki's CPU code among them) were
compiled as calls through the table, which has no Dreamcast counterpart (the
CPU Eagle crashed). `mipsgen.Function.jump_table` now takes a `.rodata`
table's entry count from the switch's bounds check (`sltiu t, index, n`).

### Versus mode's select screen

The PSP's versus grid (`0x227378`, 7 x 7) has the new characters in the
fifth row (Eagle, Maki, random, Yun, Ingrid; row 4 without its random
cells) and the World Tour entries in a seventh (its setup writes them at
cell 42 + column, `0x9C854`); its layout (`0x225A40`) has the seventh row at
y 80. The Dreamcast's row 5 is the entries, and its screen has the SPEED and
STAGE lines right under it. The add-on (`screens.py`) gives it the PSP's
rows, keeping the two random cells in row 4:

- row 5: Eagle, Maki, Yun, Ingrid in columns 1, 2, 4, 5 (grid bytes; faces
  and frames from extra lists after the layout's own, as for the arcade
  screen: `hle_layout_draw`, now keyed by the layout's base too);
- the entries go to row 6: their records' y 56 -> 80, and the code's own
  constants: `f_8C06EFAC`'s cell 35 + column -> 42, `f_8C0703CC`'s entry row
  5 -> 6, the cursor's last row 5 -> 6;
- the SPEED and STAGE lines down 24 (`0xAE`, `0xBE` -> `0xC6`, `0xD6`);
- `8C0BE45A` (a character's face part) moved, with the new faces.

Constants in the code: `chartables.IMMEDIATES` lists instructions (`mov
#imm`, `add #imm`, `cmp/eq #imm`, `mov.w @(disp,PC)`, `mov.l @(disp,PC)`)
whose value the
recompiled code reads from `EXT_IMM` (`98DFF000`) + 4 x index; the runtime
writes the instructions' own values there at boot (`char_tables_init`, the
recompiler's `g_imm_defaults`), and the add-on's patches change some.

### World Tour

World Tour's screens (the map, the setup and level-up screens) show the
character's banner: `f_8C0366D0` draws three bank `0x1D` parts from
`8C0AB7D0`[character] (a halfword table, 38 entries; the PSP's is
`0x2C89C0`, 42). Bank `0x1D` is `WORLD.PAC`: file `0x106`, loaded by
`f_8C01C508` (on the map) and `f_8C01C5E8` (the other screens) into three
buffers they allocate: `0x4000` bytes for the parts (section 4), `0x30000`
for the tiles (section 5), `0x400` for the palettes (section 6). The
Dreamcast's has 345 parts; the new characters read past the table (Yun: part
`0x176`, which doesn't exist), the tile decompressor ran on garbage and
overwrote memory: a crash right after picking one.

`worldtour.py` adds each new character's three parts from the PSP's
`world_u.pac` (bases `0x173`, `0x176`, `0x179`, `0x17C`) to `WORLD.PAC` as
`0x159`-`0x164`, with their tiles and records, and patches the moved
`8C0AB7D0`. The tiles section grows past `0x30000` (to `0x31000`, rounded
to 4 KB), so the two loaders' allocation sizes (`mov.l` literals at
`8C01C520` and `8C01C600`) are among the code's constants (Versus mode's
select screen, above). The add-on installs its `WORLD.PAC` in place of the
disc's (`build/psp_out/WORLD.PAC`).

### Endings

The arcade ending (dreamcast.md, Arcade endings) is an overlay, `ENDnn.BIN`,
and an archive, `ENDnn.PAC`. The PSP has no overlays: its ending engine is
in `BOOT.BIN` (`0x1C901C`-`0x1DC800`; its setup `0x1DB21C`, the per-frame
loop `0x1DA14C`, its steps at `0x2B3680`), its per-character directories
at `0x2B34B0` (6 words: the Dreamcast's 1, 6, 7, 8, 9 and 11) with the
data they point to, and the rest in `endNN_u.pac` (sections `0x20000`-
`0x20005`, `0x10003` the text, `0x4000019` the palettes, bank `0x19`, the
textures from `0xC17`); `endcom.pac` (file 8) the shared section 0 and
textures `0xC00`-`0xC16`. Its loader (`0x12550`) is `f_8C01BA82`'s.

`ending.py` builds `END26`-`END29`: the code, variables and routines of a
Dreamcast ending (`END06`, the one with most room), its directory where
`END06`'s is, and between them the character's data: the PSP directory's
blocks (deep-copied by the relocations, palettes' colours swapped), the
sections (the index tables `0x20003`/`0x20004` turned into the pointer
tables), the text records (the 128-byte blocks cut after their one or two
lines). The archive: `END00.PAC`'s shared sections, the PSP's own (textures
twiddled and their colours swapped, bank `0x19`'s tiles twiddled). Its
`check` rebuilds the Dreamcast's own endings from the PSP's data and
compares: they match but for the sprite numbers and a few bytes.

The PSP's engine differs from the Dreamcast's in its objects:

- people (actor type 75 and others) are drawn with the fighters' own
  sprites: every animation from the 10-byte records (`0x1D4C40`,
  `0x1D80A8`, `0x1D9448`) takes the bank of the fighter whose character the
  actor is, or for one neither fighter is (a cameo: Guile in Eagle's ending,
  Guy in Maki's, whose poses are at the end of the player's own file) the
  fighter's that isn't M.Bison; the records' pose numbers are the fighters'
  own (the Dreamcast's endings have copies in bank `0x19` instead);
- effects draw from bank 5 (`EFCT.PAC`; type 77, type 80's sub-types 2,
  11, 19...);
- types 73, 77 and 79 have more sub-types (73: 49 to the Dreamcast's 43);
  Maki's and Ingrid's endings use them;
- Ingrid's actors get her effects (`0x1D513C`, `0x1C8EC0`);
- characters' VS-screen portraits slide through scenes (Rose's, Bison's and
  Ingrid's in Ingrid's ending): type 74 (`0x1CB178`) with an animation
  number below `0x80`, and a type 73 sub-type (`0x1C96AC`), take bank `0xB`
  (the select screen's, `select_u.pac` banks 8 and `0xB`: `0x10140` loads
  it again if it's gone), the 32x32 way, and the animation from the table
  `0x223A4C` (indexed by a fighter's +`0x182` in a fight; the ending's entry
  1): `0x2237AC`, the VS portraits' records (42, one per character; the
  Dreamcast's `8C0BEB3C`, 38), whose part numbers are the PSP's bank B's.
  The Dreamcast's type 74 has only the ending's own records. `screens.py`
  already copies the Dreamcast's groups with the new characters' records
  to the extension RAM: those are the PSP groups' counterparts
  (`screen_info["psp_groups"]`, `Build.map_data`), so the copied table
  points there. The palettes are the ending's (the actor's palette base).

So the PSP's object handlers run for these endings: the build recompiles
the handlers of types 72-80 (`ENDING_HANDLERS`, as `0x1DB21C` installs
them) and what they reach (about 600 functions: they share code and tables
with Ingrid's module), with their stubs and PSP data at `98F00000` in the
extension RAM (the character files aren't loaded during an ending), and
`hle_ending_setup` (on `f_8C0218B6`, which the setup calls once its own
handlers are in) puts them in the effect objects' table. Objects the
ending makes whose handlers a character file registers during a fight
(not loaded then) get theirs too (`ENDING_OBJECTS`): Ingrid's effect
(`0x1C8EC0` makes a type 58 in pool D, handler `0x1C8F08`). The Dreamcast's
code runs the rest (the scenes, the script, the text, the textures). Both
engines' state is the same (PSP `0x87FCB0`, Dreamcast `8C1A31D4`: a manual
map); `align.py` lines the two engines up from their handlers too (its
votes for the PSP variables they use; not for the ending engine's own
functions, recompiled: where the Dreamcast's inlined a helper the PSP's
call lines up with another). The three engine functions the handlers call
with no map yet are in `manual_maps.json` (`0x1DA64` = `8C024AD4`,
`0x1DAA8` = `8C024B1A`, `0x66CC` = `8C01532A`). An ending the add-on built
marks itself (directory word 12 -> "PSPE" and the 10-byte records' range):
`hle_ending_anim` (on `f_8C024AD4`) applies the fighters' banks when the
Dreamcast's code starts such an animation, and `ending_draw` (in the sprite
hook) makes an object with a fighter's bank draw the fighters' way.

Segment 1 numbers: the ending code builds addresses below `0x2D4FC0` with
segment 1 relocations (offsets into the `.bss`, e.g. `0x2B0E8`), which read
as code: the tools followed them into some 7000 functions. `mipsgen.Boot`
now keeps each relocation's address segment and moves such numbers to
`0x1000000` + offset (`canon`, `ptr`; `psp_xa1` in the generated code); the
data the code reads in `.rodata` (the closing credits' strings) is copied
too.

The voted globals inside the object arrays (fighters, projectiles, pools)
are dropped: a region starting inside an array hid the array's translation
for the addresses after it (`0x80B408`, player 2, had none).

The closing screen (type 76, the PSP's `0x1D548C`): the Dreamcast's last
step makes it in every ending; the PSP's engine never does (no actor record
or code makes a type 76: its handler is dead code there). Its logo's part
(object +`0x54` on the PSP, +`0x44` here) comes from the PSP's ending state
+`0x138`, which `0x1DB21C` zeroes and nothing writes; the Dreamcast keeps a
struct of its own there (`8C1A330C`: Maki's ending drew her part 1, a big
blob, over PRESENTED BY). The PSP's banks have the logo as their own part
(the Dreamcast's pixels: `end26` `0x6A`, `end27` 0, `end28` 5, `end29`
none), so `ending.py` (`bank19`) finds it in the character's bank, or
copies `END00`'s into it (Ingrid's: part `0x5D`), and puts its number after
the mark; the recompiled handler reads it there (`build_char.py`
`ENDING_REPLACE`: `mipsgen.REPLACE` gives the `lw` at `0x1D54FC` other C,
`psp_ending_logo()` in `psp_load.c`). Its palette (`0x2B2978`, 16 colours,
`0x6EA8` = `f_8C015AF6` copies it to slot 0) is the Dreamcast's
`8C80AC64` with red and blue swapped: `ENDING_PALETTES` swaps them back
when the ending data is copied. The PSP's draw modes are the Dreamcast's
(table `0x200B48`; mode 3 `0x870C` the pose from part +`0x54`).

build/test/arc_end.sh `<name> <moves> <stages>`: arcade won stage after
stage, then shots through the ending; `STAGE=9` puts the stage counter at
the last, `EXTRA="SFA3_FORCE_CHAR=1:A"` makes the last opponent M.Bison.

### Installing (the setup app)

The setup app (and `sfa3port.py setup --psp`, `SFA3-Setup --convert IMAGE
FOLDER PSP-IMAGE`) takes the player's PSP image next to the Dreamcast disc.
`tools/pipeline.py`'s `addon` step, after the recompile:

1. `psp_iso.py` reads the image (ISO 9660: an `.iso`, a `.cso` (CISO,
   raw-deflated 2048-byte blocks) or either inside a `.zip`, unzipped to a
   temporary file) and writes `PSP_GAME/SYSDIR/BOOT.BIN` and
   `PSP_GAME/USRDIR/usa/data/` to `<data>/psp` (about 155 MB, kept: an
   update rebuilds the add-on from it without the image). `BOOT.BIN`'s SHA-1
   names the release (`ULUS-10062`, `3fd0abf7...`).
2. `align.write_maps` (7 s) writes `<data>/psp_maps.json`.
3. `build_char.build` (about 4 minutes, the tile conversions) writes the C
   and the files to `<data>/psp_out`; any problem fails the step.
4. The files (PAC/BIN) go to `<data>/addon`, named in `files.txt`; the C
   (`psp_*.c`) is added to the generated sources (the Zig build; CMake finds
   `build/psp_out` itself), and `psp_none.c` (in the runtime library) isn't
   linked then. `install.json` gets `"psp_characters": true`.

The game (`hle_init`): built with the add-on (`g_psp_char_data_size` not 0),
it reads `<data>/addon/files.txt` and serves each file in place of the
disc's or as a new one (`gdi_provide`: replaced if the disc has it, else
added; up to 64 such files). Without the list (and no `SFA3_ADD`) it stops
with a message: its code can't run on the Dreamcast's files (the HUD, the
select screens). Netplay's build id mixes in the add-on's data, so a game
with it only meets one with the same add-on.

A full install with the add-on takes about 9 minutes (12 threads), an update
4.5 (the add-on is converted again each time).

### The ranking

Score Ranking (Mode Menu): the lists draw each entry's character's face from
the mode menu's archive (`MSEL.PAC`, sections 7/8/9: bank `0x12`), the part
from a byte table by character (`8C0D2B01`; the other menu modules' copies
`8C0D3045`, `8C0D3D1D`), mirrored by a u16 table (`8C0D2AB2`, copies
`8C0D2FF6`, `8C0D3CCA`: its low byte to the face object's +`0xB`; the
quad's flips come from the object's +`0x54`/+`0x58` in `8C0193A0`), 37
entries each. A ranking entry (16 bytes at `8C09B33C`, 6 lists of `0x220`:
+0 the score (BCD), +4 the name, +8 the character, +13 newly entered)
naming a new character read past them: garbage sprites, then upside-down
faces. The PSP has the new faces after the others in its archive (parts
`0x80` + `0x2C8C60`[character], 42 entries: Eagle `0xB3`, Maki `0xB4`, Yun
`0xB5`, Ingrid `0xB6`) and the mirror bytes at `0x2C8C8A`; `menus.py` adds
the faces to the Dreamcast's bank (parts `0xD6`-`0xD9`; their palettes are
the Dreamcast's) and fills the six moved tables.

The portrait screen (task `8C061C7E`, step `8C062072`, drawn by `8C07EAB4`):
`f_8C01C8D0` loads `RK_COM.PAC` (bank `0x16`: name plates, part
`8C0D2035`[character] + 2) and names[90 + position] (`RKnn.PAC`, bank
`0x17`). The PSP's `rkNN.pac` are the Dreamcast's without their lead: the
Dreamcast's files start with the portrait again (192 right, 96 down: the
pose drawn from part 0), each sprite frame alone and each name strip alone
(Ryu 16 parts, Ken 19, Akuma 11), then the PSP's order (the portrait, then
name strip + frame pairs). Two animation tables by character (`8C0D1EA8`,
`8C0D1F40`; the PSP's `0x236DA8`, `0x236E50`, 42) name the pairs: the
PSP's records are the Dreamcast's with the lead taken off the part numbers.
The palettes: the Dreamcast's 176 are the PSP's 194 with groups left out
(the PSP keeps a tag in colour 0); `ranking.py` learns the index map from
the characters both have (it rebuilds the Dreamcast's own palettes 6307 of
6336 the same), builds `RK26`-`RK29` (lead + the PSP's parts; Eagle's lead
33 parts), the animations (part numbers + the lead) in the extension RAM
(`EXT_RANKING`) and `RK_COM.PAC` with the new name plates (the PSP's part
2 + `0x2C8C36`[character]). Before, a new character's rk name was the next
family's file (an OP file).

The player's score is fighter +`0x140` (binary, the HUD's; dreamcast.md). A game that ends with a ranking score is saved
with the entry (Entry Mode's name); the portrait screen didn't come up in
the arcade runs tried (cleared or lost).

build/test/rank_run.sh `<name> [commands]`: Mode Menu > Score Ranking >
Arcade, the commands run first (e.g. `poke 8C09B344 38`: the first entry's
character Eagle); `AFTER=` commands at the end (dumps).

### The CPU

The CPU plays a character by a script (dreamcast.md, CPU players). The
PSP's interpreter (`0x74A20`, table `0x21E598`) has one command more than
the Dreamcast's (81, 80), inserted as 40: a random branch, taken when
(random & 31) <= its operand, then as the other conditions do (the routine
named next, or skip to the block's `7601`/`7602`; `0x7610C` and `0x75C8C`
are the Dreamcast's `8C058CE2` and `8C058AB2` paths). Its 41-80 are the
Dreamcast's 40-79 (Dreamcast 75 and 76 share a handler, as PSP 76 and 77
do; PSP 41 is the Dreamcast's 40 instruction for instruction). The new
characters' scripts are in the PSP's numbering, so `hle_ai_step` (on
`f_8C058516`) translates for a fighter whose character is `0x26` or more:
41-80 run the Dreamcast's 40-79, and 40 runs command 38 (no operands, takes
the branch when fighter +0xEB isn't 0) with +0xEB set from the game's random
byte for the call. Before, the scripts ran the next command up from 40 on
(Eagle's and Maki's use 37 and 40) and Ingrid's lost its place and crashed.
The PSP's CPU code has no counterpart in the function map; the routine
runner `0x732C0` is `8C057520` (manual map).

build/test/cpu_fight.sh <hex char> <name>: arcade as Ryu with player 2 (the
CPU) forced to the character (`SFA3_FORCE_CHAR=1:`), 40 s of the fight.

### Function map fixes

The alignment (`align.py`) had paired `0x67B1C` with the animation stepper
`8C04C29C` (no return value) and `0x67E50` with `8C05082C`. A stand loop
(Yun's `0x1BBDF4`) calls `0x67B1C` and goes to walking when it returns 1:
with the stepper in its place, a stand entered with the lever neutral ended
its one-shot animation every frame and never walked. Round 1 hid it (the
stand is entered with the lever held after the intro); from round 2 on the
character didn't move. When a function's result is used, check that its
counterpart returns one.

Round reset: the Dreamcast's `f_8C0253F8` resets a fighter for a round; the
PSP's (`0x1EAEC`) also clears the PSP-only word at +0x38, which here lives in
the side storage (`psp.c`): `hle_fighter_reset` clears it after the original.

Recompiled from the PSP: memcpy (`0x1EF430`), small wrappers (`0x670AC`
Maki, `0x676A8`), `0x66890` (Ingrid: walks the fighters through the PSP's
fighter pointer table `0x213F88`), `0x1DC324` (an event/demo system Ingrid's
extra code calls), and the PSP-only command checker `0x64220` (Yun, command
0x20) with its two handlers (`0x642AC`, `0x64368`, table `0x217D30`) and its
button-configuration helpers (`0x634D4`-`0x63C30`).

Game state: PSP `0x39C9E0` has the Dreamcast's `8C17FBC0` layout below +0xA0
(the alignment votes agree; +0x72 is the game mode on both). Above that the
Dreamcast keeps another block (PSP `0x8082F8`).

### Building: what gets copied

A PSP data object the recompiled code needs and the Dreamcast doesn't have is
copied into the character files whole: from its address up to the next
address the PSP program refers to (relocations list them all). Pointers in
it are followed two levels; code pointers in it are recompiled when they're a
character's code or, in a table the code reads directly, engine code with no
counterpart. Switch tables are left out (the recompiled code has them built
in). PSP-only variables (in `.bss`) get zeroed room in the extension RAM.

Ingrid's code ends at `0x1CB124` and her data at `0x2B2310`: a PSP-only
module follows each (its dispatcher `0x1CB124` reads the table at
`0x2B2310`). Yun's own descriptor builder `0x1BF93C` is a no-op at run time
like `0x6AB48` (the build runs it).

### The pause menu's ATTACK LIST

The rows shown come from the character's move-list texture and a record
picked by `8C0D5064`[character] (38 pointers; `8C08FA32`): bytes of texture
row numbers by scroll position and line, one of nine shared records
(`8C0D4EFA`-`8C0D502A`) chosen by the list's length. The PSP's table
(`0x238EC8`, 42) points into the same nine records (`0x238D5E`-`0x238E8E`,
byte for byte the same), but it lengthened some of the old characters' lists
(22 of 38 entries agree), so the moved table maps the PSP's record to the
Dreamcast's of the same rank (chartables kind `ptr_rank`): Eagle the second,
Maki the seventh, Yun and Ingrid the eighth. Without it the new characters'
rows were whatever followed the table (every line showing the banner).

The English move lists (`movelist.py`), all from the player's discs:
- The font: the Dreamcast's English lists' name lines (`SAMPLES`: 21
  characters' names, in the order of their 10x2 parts), cut into letters
  where a column has no letter pixel (a letter's own pixels, not its shadow:
  shadows touch the next letter), kept where a name's pieces match its
  letters (a pair that touches, like "ky", is skipped); each letter the
  shape most samples give. `+ - : , / ( )` (not in any name) are drawn in
  the same style. Gaps: 2 columns letter to letter, 8 for a space.
- A move's name: left at x 1, up to the -ism icons; a name too wide comes
  with the letters closer, then squeezed.
- The command: the PSP line's cells with colour in them (motions, buttons:
  `icon()`) kept in order, the white ones (Japanese) dropped; `LISTS` gives
  each move's line as text with `@` for those cells (the counts checked),
  each piece of text in cells of its own.
- The -ism icons: the Dreamcast's cells showing the same letters (by
  colour: X red, A green (the PSP's Z), V blue, LV3 yellow; the icons
  straddle cells), from a list whose name doesn't run into them.
- The tabs: Ryu's (MISC. / SPECIAL TECHNIQUES, SUPER COMBOS).
- A new texture of the cells used (139-201 of 256) and the parts again.

### Pre-fight dialogues (arcade)

A dialogue is one of the player's scripts: descriptor word 38's entry 0 is a
table of scripts, each a list of u16 (text number + 1 in bits 0-9, bit 15
the speaker's side), the text numbers indexing entry 1 (the lines; for a new
character the PSP's `bdNN_u.pac` section `0x10001`, `build_char.dc_text`).
The stage object runs it (`8C0A8450`: 6 states, `f_8C02CBB6` the lines);
its +0x42 is the script, set from per-character tables (`0xFF`: none):

| Table | Picked by | Read at | PSP |
|---|---|---|---|
| `8C0A8468` | character | `8C0270EA`, `8C027194` | `0x20B508` (42) |
| `8C0A8490` | occasion (stage object +0x3B - 4: 0-2) x 40 + character | `8C0272A0` | `0x20B532`, rows of 42 |
| `8C0A8508` | occasion (0-1) x 40 + character | `8C028BD6` | `0x20B5B0`, rows of 42 |

The Dreamcast's rows are 40 bytes (38 characters, 2 spare): the new
characters read the spare bytes and the next row's first (Maki got none of
hers: `FF` padding; Yun Ryu's row's numbers). Moved, each row is a table of
its own, 44 bytes apart, and the code's row x 40 becomes row x 44
(`chartables.REPLACE`: the `shll` at `8C0272A2` and `8C028BDA` replaced by
C in the recompiled code, `codegen.REPLACE`). The new characters' entries:
`8C0A8468` Eagle 3, Maki none, Yun 4, Ingrid 0; `8C0A8490` rows 0 / 1 / 2:
all 0 / all 2 / 4 4 5 4; `8C0A8508` rows 0 / 1: all 1 / none, 3, 3, 3.

The speakers' faces are objects of type 14, id 6 (`8C06A8BA`), one per
fighter, spawned by the stage object's first state: the face is a
`SELECT.PAC` bank 8 part (a chain) from `8C0C0E04`[character] (u16,
`8C06A948`), its palette the VS portrait's (`f_8C069D34`). The new
characters read past the table (`0x8000`: nothing, for Eagle and Maki): the
moved table gets the PSP's (`0x226098`; its part numbers are the
Dreamcast's less `0x3B`), the four faces (PSP parts `0x2E7`-`0x2EA`, 96x48)
added to bank 8 (`screens.py`).

The dialogue's extra sprites are bank `0x1A` from `BDnn.PAC`: tiles at
`8CA30000`, parts at `8CA3E000` (the banks' default places `8C09AF10` /
`8C09AE80` + 4 x bank; the parts load first). The PSP Yun's tiles are 86047
bytes against the Dreamcast's biggest 54278: they ran over his parts and on
to `8CA4501F` (a crash later in a run). The add-on moves the bank's parts'
default place to `8CA45800` (`build_char.bd_parts`), within a ranking
screen bank's room (`0x16`, up to `8CA49200`) that a fight doesn't use.
Moving the tiles to the extension RAM instead crashed at once: the fight's
sprite code wants them in the main RAM.

## Sound (`tools/psp/sound.py`)

The voice numbering lines up: every character the two have in common has as
many voice samples on the PSP as on the Dreamcast (Chun-Li, 0x09: 31 and 32),
in the same order, and the voice records (section `0xD`) have the same
format. So a new character's voice bank is its PSP samples, converted:

- PSP: section `0x40000002` `PPHD` (+0x18: the offset of its `PPVA` chunk:
  u32 size, 16, -1, 0, u32 entries - 1, -1, -1, then per sample offset,
  rate, size, -1; rate and size -1 for an empty entry) and `0x40008002`, the
  VAG data (Sony ADPCM, a silent 16-byte frame first). Rates 22050, 20182,
  16000 and 11025 Hz.
- Dreamcast: a `SOSB` voice bank, the `SMPB` KO echo bank and the records
  (dreamcast.md, Sound). Each player's banks have `0x48000` and `0x5000`
  bytes (`ZERO3.MLT`).
- Room: Eagle's voices fit as they are (240 KB); Maki's, Yun's and Ingrid's
  come to 335-352 KB as AICA ADPCM. Each sample is cut to its sound (the
  silent first frame, the tail under 48), the KO scream (sample 0) to 1.8 s
  (the echo bank holds it too), and then every sample above 12 kHz is played
  at a lower rate, as little as fits: Maki 0.855 (22050 Hz -> 18853), Yun
  0.88, Ingrid 0.89; a sample longer than 65535 (the AICA's end register)
  lower still. The PSP's KO screams for the Dreamcast's characters are the
  Dreamcast's with the echo mixed in (36412-36437 samples longer); the new
  four's have no such tail.
- Records of type 4: the PSP's sound queue (`0x5A4C`) turns them into type 0
  on channel + 8 (a separate slot), a nonzero bank + 2 (Eagle's sample 28 and Yun's
  26-36 play from their own banks so); the Dreamcast's
  queue waits forever on them (the hang at Eagle's round-win pose). They're
  written as type 0 on a channel offset (0-2) the character's other records
  don't use; Ingrid's only stop, so they become none.
- Level: for the characters both have, a voice sample's RMS on the PSP is
  1.53 times the Dreamcast's (the median; Ryu, Ken, Sagat, Juli alike, from
  1.0 to 1.9): the new characters' samples are scaled by 1 / 1.53
  (`sound.GAIN`). Unscaled they came out twice as loud as the Dreamcast's.
- The common sounds (`SD_SE.BIN`, `SD_NR.BIN`; the PSP's `se_mid.pac`
  `0x80000D00`, `0x80000E00`) are the same apart from a few entries that
  point at the same sounds in other banks; the new characters use the
  Dreamcast's.

Checked with `SFA3_KEYON_LOG` (every voice start: address, length, pitch)
and build/test/keyons.py, which matches them against the converted bank.

## Palettes

A character's 6 colour choices have 5 palettes each on the Dreamcast (30):
a player's palette base (`8C0B21AC`[mode x 4 + side], fighter +0xD) is 0,
5 or 10 in the 64-palette buffer `8C1A2174` (uploaded by `f_8C015D60`;
slots up to 43 and 56-63 are in use in a fight). The PSP's bases are 0,
0x40, 0x48 (`0x215622`), and Ingrid has 8 palettes a choice (48): the
Dreamcast's choice c read her palettes 5c..5c+4, a mix of two of hers (her
hair red). `files.palettes` keeps each choice's first 5: her sprites use
sub-palettes 0-4 but for part `0x2A4` (sub-palette 5, a blue streak), and
her effect objects add small offsets to her base (`p_1C7974`).

## Gaps

- The PSP's stages and music.
- The rest of the character tables (Character tables, above).
- A ranking entry or saved data naming a PSP character would read past the
  Dreamcast's tables without the add-on.

## Debugging switches (environment)

| Variable | What |
|---|---|
| `SFA3_REPLACE=NAME=path;...` | serve a file's contents from a host file |
| `SFA3_ADD=NAME=path;...` | add files to the disc |
| `SFA3_FORCE_CHAR=player:hex` | write the character number every frame |
| `SFA3_PSP_MUTE=1` | skip the PSP code's sound calls (not the animations' sounds) |
| `SFA3_KEYON_LOG=1` | log each AICA voice start (address, loop, end, format, pitch, envelope, volume) |
| `SFA3_PCM_DUMP=file` | write the mixed sound output (44.1 kHz stereo 16-bit) |
| `SFA3_PSP_TRACE=addr` | trace one PSP function (`0`: all, from `SFA3_WATCH_FROM`) |
| `SFA3_PSP_STUBS=frame` | log stub calls from that frame |
| `SFA3_FILE_LOG=1` | log files opened by name, with the caller |
| `SFA3_RENDER_LOG=frame` | log the render from that frame |
| `SFA3_WATCH_WRITE=addr,len` | log writes to guest memory (with the writing function) |
| `SFA3_WATCH_FROM=frame` | start watching or tracing at that frame |
| `SFA3_DMA_LOG=1` | log DMA |
| `SFA3_PAC_LOG=1` | log each archive section as it loads (tag, size, where to) |
| `SFA3_DISC_DELAY_MS=<ms>` | each disc read takes that long (testing a slow disk's stalls: the audio streams) |
| `SFA3_SPRITE_LOG=frame` | log that frame's sprite draws: object, bank, part, record, position, animation, caller (`frame+`: from that frame on, 400 lines at most) |
| control `rtdump <file>` | the read trace (read-trace build only) |
| control `poke <hex address> <decimal value>` | write a guest byte |
| control `fill <hex address> <bytes> <value>` | write guest bytes (decimal count and value); the control queue holds 512 lines |
| control `untilnz <hex address> [frames [button]]` | hold the queue until the guest u16 there isn't 0 (at most frames, default 6000), tapping player 1's button every 40 frames meanwhile |
| control `untileq <hex address> <value> [frames [button]]` | the same until the guest byte is value (decimal); `untileq 8C17FBC6 3`: a round is on, `4`: it's over |
| control `hold <1\|2> <buttons\|none>` | keep buttons held (`right+y`; `l`, `r` the triggers) |
| control `dump <hex address> <bytes>` | log guest memory |
| control `mark <file>` | create the file: the commands queued before it are done |
| `SFA3_CTL_LOG=1` | log each control command with its frame (the queue holds 512) |
| `SFA3_FORCE_HEALTH=<fighter>:<value>` | keep fighter 0/1's health at most value (1: a hit wins the round) |
| `SFA3_POKE_EVERY=<hex address>:<value>,...` | write guest bytes every frame (up to 8) |
| `SFA3_MMIO_BT=<hex address>` | the host call stack of the first access to that 16 MB area |
| `SFA3_SPRITE_FIND=<hex part>` | the first 20 draws of that sprite part, any bank |
| `SFA3_SPRITE_NEW=1` | each (object type, sub-type, bank, part) the first time it's drawn, with the frame, object, character, palette and position (4096 at most) |

## Test scripts (`build/test`, not in the repository)

`run.sh` (instance 7; `BIN=` another binary), `id_fight.sh <id>` (character
`<id>` forced for player 1, files added with `SFA3_ADD`, Versus against
M.Bison), `char_test.sh <id> <name>` (every motion with punch and kick, a
screenshot and the fighter's state +4..+7 after each), `trace_run.sh <id>
<out>` (a fight with the read-trace build, then the trace),
`eagle_fight.sh`, `motion.sh` (inputs: qcf, qcb, dp, rdp, hcf, hcb, qcf2,
qcb2, mash), `movestrip.sh`, `shots.sh`, `alive.sh`, `mkpac.py`
(a PSP archive in the Dreamcast's form), `idtables.py`, `desccmp.py`,
`newchars.py`.

The add-on's whole flow: `rebuild.sh [norecomp]` (recompile, `build_char.py`,
runtime), `addon_files.sh` (`SFA3_ADD`'s list for `build/psp_out`),
`asel.sh "<moves>" <seconds> <name>` (arcade from the title with the add-on;
grid cell (5,0) is Yun: `poke 8C0BE3EF <decimal>` puts another character
there), `flow_trace.sh <poke|none> <out> [frames]` (the same through the
prologue, VS screen and into the fight with the read-trace build, then the
trace), `scan_tables.py` (Dreamcast tables the PSP extended), `tabinfo.py
<addr> <size> [count] [PSP addr]` (a table on both machines),
`pspxref.py <addr>` (PSP code building an address), `pdis.py <addr> [n]`
(PSP disassembly).

Arcade runs: `arc.sh <name> [select moves] [after]` (the add-on's arcade
from the title in batches of control commands: select, the prologue, a fight
won by KOs, then after's commands; env `B3`, `FIGHT`, `R1`/`R2`, `ONE_KO`,
`KO_OFF`, `AFTER_S`; a montage of the shots in the scratchpad, the shots then
deleted), `arc_full.sh <name> <moves>` (one character all the way to the next
stage with shots throughout: the fight follows the round state with
`untileq`, the script waits on `mark`; `EXTRA=` more environment).
`char_test.sh <id> [name]` loads the add-on's whole set of files
(`addon_files.sh`; its PITU.PAC goes with the runtime's HUD patches;
`PLPAC=` another character file). `keyons.py <hex char> [player]` matches
the keyon log against the character's converted voice bank. Select moves from the cursor's start
(Ryu, row 3 column 0): Yun `down down`, Ingrid `right` x6 `down down`, Eagle
`right right down down down`, Maki `right` x4 `down down down`.
`vswin.sh <hex char> <name>` (Versus, two KOs, the win screen),
`cpu_vs.sh <opponent> <seconds> <name>` (arcade as Ryu against a poked first
opponent: the CPU playing a character). `hangbt.sh [out]` waits until the
test game stops rendering for 6 seconds and writes gdb's backtraces (the
recompiled functions by name); gdb can also stop on a guest condition, e.g.
`break f_8C01421E if (*(unsigned*)((char*)&g + 16) & 0xF00) == 0x400`
(`g.r[n]` is at `&g + 4n`, `g.pr` at `&g + 148`).
