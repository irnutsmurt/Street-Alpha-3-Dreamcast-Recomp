# Dreamcast SFA3 (US, T-1203N v1.001): reference

What the port knows about the game: memory map, structures, functions and
file formats. Addresses are SH-4 P1 addresses (`8C......`); functions are
named as the recompiler names them (`f_8C01AE28` is the function at
`0x8C01AE28`). "Unconfirmed" marks things read off the code but not tested.

The main program is `1ST_READ.BIN`, loaded at `0x8C010000`. Character code
comes in overlays (`PLnnA/B/C.BIN`, one per player slot) at `0x8C800000`,
`0x8C814000` and `0x8C828000` (`0x14000` bytes each).

## Memory map

| Address | What |
|---|---|
| `8C010000` | `1ST_READ.BIN`: code, then data (tables from about `8C094000`) |
| `8C09895C` | file name table: file number -> pointer to a name (`pl00.pac` is file 13) |
| `8C09ABE8` | stage number -> file number (bytes) |
| `8C09B040` | overlay name template `PL00A.BIN` (digits at +2/+3, slot letter at +4) |
| `8C09B04C` | overlay slot addresses (`8C800000`, `8C814000`, `8C828000`) |
| `8C09B068` | (`[8C17FC98]` x 4 + fighter +0x131) -> overlay slot (bytes; `f_8C01AE28`) |
| `8C09B116` | character -> `bdNN.pac` file number (u16, 40 entries) |
| `8C09B1CA` | unlock bits (`0x800` Final Bison); saved in `SFALPHA3.SYS` |
| `8C0A8578` | character names (38 pointers; see Characters) |
| `8C0A9278` | per-character jump table the overlays patch (see Descriptor word 29) |
| `8C0B1BDC` | (mode, player) -> overlay slot (bytes) |
| `8C0B1C24` | fighter action categories (fighter +5): handlers, 9 entries |
| `8C0B369C` | command checker state handlers |
| `8C0B39C8` | command definitions: command number -> pointer to a u16 sequence |
| `8C0BA64C` | (game mode, side) -> sound channel (bytes) |
| `8C142B10` | Kamui: render state |
| `8C17FBC0` | game state block (see Game state) |
| `8C17FC60` | second game state block (the PSP keeps it apart: `0x8082F8`) |
| `8C17FC98` | a mode number (s16) indexing per-player tables (`8C09B068`, `8C0BA64C`) |
| `8C17FC9B` | 2: a CPU fighter's files are character `0x15`'s (`f_8C01AEC8`) |
| `8C17FD38` | fighters: 4 x `0x44C` bytes (to `8C180E68`) |
| `8C180E68` | projectiles: 24 x `0x148` bytes (two pools of 12; to `8C182D28`) |
| `8C1832A8` | effect objects: 80 x `0xB0` bytes (pools of 32, 16, 16, 16; to `8C1869A8`) |
| `8C186AB8` | a pool of 3 x `0x30` (allocator `8C0256DC`) |
| `8C1870E8` | menu sprite objects, `0x90` bytes (Option Mode buttons are kind 12) |
| `8C1882E8` | free counters for the object pools (one byte each) |
| `8C1888B4` | camera / scroll block; +0x10 the camera X (16.16) |
| `8C188D48` | World Tour entries, 2 x `0x354` |
| `8C1A1F48` | sprite parts table per sprite bank (pointers) |
| `8C1A1FE8` | sprite tiles per sprite bank (pointers) |
| `8C1A2088` | palettes per sprite bank (pointers) |
| `8C1A2128` | per-player sound bank pointers |
| `8C1A2144` / `2150` / `215C` | sprite parts / tiles / palettes per player slot (pointers) |
| `8C1A2170` | character data area: where the next section goes (offset from `8C347300`) |
| `8C1A2174` | palettes in use: 64 slots of 16 colours (`f_8C015AF6` copies into them); `8C1A2974` = changed |
| `8C1A3F9C` | sound command queue: 8-byte entries; head `8C1A419C`, tail `8C1A419D` (bytes) |
| `8C1A3730` / `8C1A3734` | file loader state; 120-byte load records |
| `8C2F5300` | disc read buffer |
| `8C347300` | character data area: `0x140000` bytes per player slot, 3 slots |
| `8C72A300` | sprite tile staging (16 tiles of 128 bytes, then uploaded) |
| `8C72AB00` | one unpacked sprite tile |
| `8C76E808` | Kamui: rendering in progress (the frame waits for it, with a timeout) |
| `8C800000` | overlay slots A/B/C (`0x14000` each) |
| `8CD6CF20` | file data buffer for whole-texture loads |

## Game state (`8C17FBC0`)

| Offset | What |
|---|---|
| +0, +2 (u16) | routine numbers; +2 = 1 on the select screens, 2 in a fight |
| +3 | Option Mode cursor |
| +4, +6 (u16) | screen sub-state; in a fight +6 is 2 the round's intro, 3 the round, 4 round over |
| +7, +8 | Option Mode: the open screen + 1, a screen is open |
| +0x21 (`8C17FBE1`) | frame counter (one per game frame) |
| +0x72 | game mode (1 Versus, 3 World Tour, 6 Team, 8 Dramatic, 12 Network, 13 VS Dramatic) |

## Fighter (`8C17FD38` + n x `0x44C`)

| Offset | What |
|---|---|
| +0 | active (1) |
| +1 | drawn |
| +4 | state: 1 in play |
| +5 | action category (`8C0B1C24`): 0 normal, 7 round intro |
| +6, +7 | step and sub-step within the action |
| +0xB | facing |
| +0xC | palette mode (signed): <0 base + tile palette, >0 fixed, 0 tile palette |
| +0xD | palette base (player 1: 0, player 2: 5) |
| +0x10 | X, 16.16 (integer part at +0x12) |
| +0x16 | Y (integer) |
| +0x1C | current animation record (10 bytes, see Animation) |
| +0x30, +0x34 | display tree links (`f_8C017728`) |
| +0x3A | animation frame countdown |
| +0x46 | sprite bank (`8C1A1F48` index) |
| +0x4C | opponent |
| +0x60 / +0x62 | health / full health (144); below 0 is a knockout |
| +0x131 | player side, it seems (indexes `8C09B068` with `8C17FC98`) |
| +0x132 | character number (the select screen writes it here) |
| +0x140 | World Tour EXP (copy) |
| +0x14E | super meter (0x90 full) |
| +0x155 | CPU-controlled |
| +0x162 | -ism / version byte (also used by the file choice in `f_8C01AEC8`) |
| +0x180 | the character's descriptor in RAM (39 words, see Descriptor) |
| +0x196 | Final Bison chosen |
| +0x20C / +0x20D | guard meter size / guard damage |
| +0x2D0, +0x2DC | lever input (u16; bits: 1 back, 2 forward, 4 down, 8 up) |
| +0x2E7 | pad |
| +0x32C | command input state: 8-byte blocks, one per command |

Projectiles are laid out like the first `0x148` bytes of a fighter. Effect
objects (`0xB0`) share the first part too.

## Characters

The name table at `8C0A8578` (38 pointers) gives the numbers:

| | | | | | | | |
|---|---|---|---|---|---|---|---|
| 00 Ryu | 01 Ken | 02 Akuma | 03 Charlie | 04 Chun-Li | 05 Adon | 06 Sodom | 07 Guy |
| 08 Birdie | 09 Rose | 0A M.Bison | 0B Sagat | 0C Dan | 0D Sakura | 0E Rolento | 0F Dhalsim |
| 10 Zangief | 11 Gen | 12 Chun-Li | 13 Gen | 14 Sodom | 15 Balrog | 16 Cammy | 17 E.Ryu |
| 18 E.Honda | 19 Blanka | 1A R.Mika | 1B Cody | 1C Vega | 1D Karin | 1E Juli | 1F Juni |
| 20 Guile | 21 Fei-Long | 22 Dee Jay | 23 T.Hawk | 24 S.Akuma | 25 Balrog | | |

`12`, `13` and `14` are second files for 04, 11 and 06: `f_8C01AEC8` turns
04 and 06 into 12 and 14 when fighter +0x162 is negative, and `f_8C01AE28`
loads 11's code for 13. The PSP adds 26 Eagle, 27 Maki, 28 Yun, 29 Ingrid.

## Characters' files and loading

- `f_8C01B25C` loads a fighter: `f_8C01AEC8` (the file character number:
  fighter +0x132 with special cases), then `f_8C01B598(player, character,
  slot)` for the archive and `f_8C01AE28` for the code overlay.
- The archive's name: `names[13 + character]` (`8C09895C`), opened at
  `f_8C01B598` (return address `8C01B648`).
- The overlay's name: the template at `8C09B040` with the character number's
  hex digits and `'A' + slot`.
- `f_8C012C80(name, ...)`: open a file by name (queues the read).
- `f_8C01348E`: an archive's next section (the load record in r4: +44 where
  to, +48 size, +100 tag, +104 sprite bank, +105 player slot). Sections
  0x100000 (sprite parts), 0x200000 (tiles) and 0x400000 (palettes) go to
  `8C347300 + [8C1A2170]` and their addresses into the per-slot and per-bank
  tables; the offset is set to slot x `0x140000` when a player's load starts
  (`f_8C0132E2`) and advanced (rounded up to 4) as each section arrives
  (`f_8C01380C`).

## Overlay (character code)

The engine calls the overlay's first address with r4 = the fighter
(`f_8C046D6E`, slot from `8C0B1BDC`). The entry copies the character's
descriptor (39 words) to `*(fighter + 0x180)`.

### Descriptor (39 words)

| Word | What |
|---|---|
| 0-3 | normal moves per button: tables of pointers to animation records |
| 4, 6, 8, 10, 11 | data tables |
| 5, 7, 9 | tables of pointers (7: per -ism, read at +4 + ism x 4; 9: by the opponent's character, 38 lists of where a held opponent goes frame by frame: `f_8C05206E`, `f_8C0520F0`, `f_8C052180`, `f_8C052268`) |
| 12-15 | data tables (pointer tables) |
| 16-27 | code: 18 the per-frame state dispatcher, 19 the round intro, 20 the command check (special moves) |
| 28, 29, 30 | (type, handler) lists ending in -1: as a fight starts (`f_8C046D6E`) each handler goes into the engine's dispatch table for that kind of object, 28 into `8C0A9334` (projectiles), 29 into `8C0A9278` (effect pool D), 30 into `8C0A900C` (word 30 often points 12 bytes before the descriptor: `[8, code, -1]`). Each character has its own type numbers |
| 31 | engine function `8C04B378` |
| 32-34 | palettes |
| 36 | u16 list |
| 38 | `[table, dialogue, data, palette]` (two or four entries); dialogue: pointers to records `[x?, y?, text\0, ...]` |

### Animation record (10 bytes)

+0 u16 duration, +4 u16 pose (a sprite part number; `0x8000` set: nothing
drawn), the rest unknown. The engine steps animations in `f_8C04C29C`.

## Sprites

- A pose is drawn as a run of 12-byte part records at bank parts + pose x 12
  (`f_8C01788E`, bank parts from `8C1A1F48`); the record whose +10 is 0 is
  the last. Each goes to `f_8C018E9C` (unconfirmed: +0 the offset of its tile
  list, +4 / +5 its width / height in 16-pixel tiles).
- A tile list: 3 bytes per tile (`hi`, `lo` of the tile
  number, attribute: palette in the low 5 bits); `FF` = an empty tile.
- `f_8C01A314` streams a part's tiles: unpacks each (`f_8C0101AC`) into
  `8C72AB00`, copies it (`f_8C0DC9E8`) into the staging area, uploads every
  16 (`f_8C0E00B0`).
- `f_8C018E9C` draws a part; palette = fighter +0xD + (attribute & 31) when
  +0xC is negative.

### Tile compression (`f_8C0101AC`)

128 bytes out (16 x 16 pixels, 4 bits each). A flag byte gives 8 items, low
bit first: 0 = a literal byte; 1 = a control byte `dddd llll`: d > 0 copies l
bytes from d back (may overlap), d = 0 repeats the next byte l + 1 times.
The tiles are twiddled (PowerVR order: y bits in the even positions, x in
the odd).

### Palettes

16-bit ARGB1555, 16 colours each; archive section 0x400000 holds 30 (6
colour choices x 5).

## Archive (`PLnn.PAC` and others)

u32 section count, u32 offset of the last section, then (tag, size) pairs
from offset 8; the header is `0x800` bytes and each section `0x800`-aligned.

| Tag | What |
|---|---|
| `00100000` | sprite parts |
| `00200000` | sprite tiles: u32 offsets (one per tile), then the packed tiles |
| `00400000` | palettes (`0x3C0`) |
| `0000000A`, `0000000D` | (unknown) |
| `10000E00` | a PowerVR texture (GBIX + PVRT, 256 x 256 ARGB1555 twiddled) |
| `40000002` | sound: `SOSB` bank |
| `20000005` | sound: `SMPB` bank |

## Commands (special move input)

Descriptor word 20 runs the checks: `f_8C04EBDC(fighter, state block,
command)` with the state blocks at fighter +0x32C. Command definitions
(`8C0B39C8`): u16 directions (1 back, 2 forward, 4 down, 6 down-forward,
5 down-back; `0x400` marks the last), then a button mask (`0x4070` punches,
`0x8380` kicks). 01 QCF+P, 02 QCF+K, 03 QCB+P, 04 QCB+K, 05 DP+P, 06 DP+K,
07 RDP+P, 08 RDP+K, 11 HCF+P, 12 HCF+K, 17 QCF x2+P, 18 QCF x2+K, 1A QCB x2+P,
1B QCB x2+K (some unconfirmed).

## Objects

Allocators: `f_8C02554E(fighter, ...)` projectiles (pool by fighter +105),
`f_8C02564C` / `25670` / `25694` / `256B8` effect pools A-D, `f_8C0256DC`
the small pool. Each takes (kind, id) and returns the object (or 0).

Each pool's objects run every frame through a table by their type (+2):

| Dispatcher | Objects | Table | Entries | Filled by |
|---|---|---|---|---|
| `f_8C030120` | effect pool A, `8C1832A8`, 32 x `0xB0` | `8C0A900C` | (to `8C0A9278`) | descriptor word 30 |
| `f_8C030170` | effect pool D, `8C185EA8`, 16 x `0xB0` | `8C0A9278` | 47 | descriptor word 29 |
| `f_8C0301C0` | projectiles, `8C180E68`, 12 x `0x148` | `8C0A9334` | 43 | descriptor word 28 |
| `f_8C0302F4` | effect pool C, `8C1853A8`, 16 x `0xB0` | `8C0A93E0` | 12+ | the program (stage objects: types 2-11 `8C03D33C`...) |

The tables sit end to end, so a type past a table's end lands in the next
one (see psp-characters.md, Character tables).

## Action categories (fighter +5)

`8C0B1C24`, 9 handlers. Category 4 (`f_8C04B338`) and 5 (`f_8C04B358`):
while fighter +0x2E4 is 0 the character's own code runs (descriptor word 18
and word 31); once it isn't, the common `f_8C04B3F2`, which sets bit
(1 << fighter +0x131) in `8C17FC6B` every frame and runs
`8C0B1F3C`[+0x2E4]. The round-over state waits for that byte: a fighter's
win (or KO) sequence is done when its bit is set. `f_8C04D3AC` puts the
winner into category 4 (and clears +0x294, the win pose a held button
picks: 1-6).

`f_8C0246A4`: the random number, a signed byte (`8C17FBC0` +0x40 its state).

## Sound

`f_8C059F2C(object, side, sound, ...)`: sound number (high byte: category;
2 = voice, uses the player's bank `8C1A2128`), pan from the object's X less
the camera's. It queues a command (`f_8C014094`) on `8C1A3F9C`; the main
loop's `f_8C01424C` sends them to the sound CPU and waits on some (an entry
reads `FFFF` when done).

Sound queue: 64 entries of 8 bytes at `8C1A3F9C` (u16 command, u16
channel, two u16 arguments), read index `8C1A419C`, write index `8C1A419D`
(u8, & 63), added by `f_8C01421E(command, channel, a, b)`; cleared by
`f_8C014034`/`f_8C014FF0`. `f_8C014094(bank, channel, sound, pan)` looks the
sound up: `8C1A3F7C`[(sound >> 8 & 15) + bank] points at 16-byte records,
record = that + (sound & 0xFF) x 16 (+0 u16 command word, `FFFF`: none; +2
channel offset; +8 volume; +10 pan / flags), and queues a group: commands 2,
8 (+8), 9 (pan), A, then 1 (play), each `(record's high byte << 8) |
command`. `f_8C01424C` handles entries whose type (bits 8-11) is 0
(`f_8C014384`) or 1 (`f_8C014454`); any other type it waits on until the
entry reads `FFFF`, which nothing writes: a record of another type hangs the
game (the main loop spins inside `f_8C01424C`, nothing renders). The
Dreamcast's own data never has one; a sound number past a bank's records can.

A fighter's animation can carry a sound: `f_8C04C15A(fighter, animation)`
sets the animation and goes on into `f_8C04C1EE`, which plays sound
(fighter +0x114)[fighter +0x122] (u16 list from the character's data, the
index from the animation) through `f_8C059F2C`.

Record tables: `8C1A452C` + 0x800 x n, n = 0..4 (128 records each): 0
`SD_SE.BIN`, 1 `SD_NR.BIN` (the common sounds), 2-4 the voices of players
1-3 (a character file's section `0xD`, 64 records: voice k is record k,
bank 3 sample k). A record's bank byte 3 is the player's (`f_8C014094` adds
the bank argument, `8C1A2128`[side] for voices). The channel is the
record's +2 plus a per-side base (`8C0BA64C`: 0 for player 1, 3 for player
2), at most 7.

Sound memory (`ZERO3.MLT`, the Katana multi-unit file: 14 units of 32
bytes: tag, bank, AICA address, size, offset in the file or -1, size in
the file):

| AICA | Unit | Size | What |
|---|---|---|---|
| `0x018000` | SMPB 0 | `0x23280` | music instruments |
| `0x03B280` | SMSB 0 | `0x9C0` | sequences |
| `0x03BC40` | SOSB 0 | `0x26E80` | common sounds |
| `0x062AC0` | SOSB 1 | `0x4F840` | common sounds |
| `0x0B2300` | SOSB 2 | `0x57940` | common sounds |
| `0x109C40` | SMSB 1 | `0x100` | the KO echo's sequences |
| `0x10A000` | SOSB 3 | `0x48000` | player 1's voices (from the character file) |
| `0x152000` | SMPB 1 | `0x5000` | player 1's KO echo instrument |
| `0x157000` | SOSB 4 | `0x48000` | player 2's voices |
| `0x19F000` | SMPB 2 | `0x5000` | player 2's KO echo |
| `0x1A4000` | SOSB 5 | `0x48000` | player 3's voices (Dramatic Battle) |
| `0x1EC000` | SMPB 3 | `0xFFC0` | |
| `0x1FBFC0`, `0x1FDFE0` | SPSR 0, 1 | `0x2000` | stream buffers |

A character file's sound sections are staged at `8CD31740` and go to sound
memory in whole 32-byte blocks: the sections are padded to them (the banks'
own size fields aren't). Formats (the sound driver is MANATEE.DRV):

- `SOSB` (one-shot bank): "SOSB", u32 1, u32 size (to the end of "ENDB"),
  u32 count, count u32 offsets of `0x38`-byte "SOSP" records (an AICA
  voice's registers: +4 address bits 16-22 and the format (`0x0100`: ADPCM,
  `0x0200` looping), +6 address bits 0-15 (from the bank's start), +8 loop
  start, +0xA end (samples), +0xC/+0xE envelope, +0x10 pitch (OCT/FNS:
  `0x7800` is 22050 Hz), +0x16 direct send level, filter, +0x2C/+0x30 loop
  start and end again, "ENDP" at +0x34), "SOSD", the samples (4-bit AICA
  ADPCM, low nibble first, 4-byte aligned, padded with `0x80`), "ENDD",
  "ENDB".
- `SMPB` (MIDI program bank): the voice banks' KO echo bank is one program
  with one split, the split record at +0xEC (its sample at +0x11C, the end
  at +0xF2), the sample the voice bank's first (the KO scream), "ENDB".
- `WINnn.PAC` section `0x40000000`: one SOSB (16 samples) for every
  character.

## File names

`8C09895C`: 271 pointers to file names. `f_8C0126F4(number)` opens
names[number] (most loads); `f_8C012C80(name, ...)` opens by name. A
character's files come in families: `pl` archive (13 + character), `st`
stages (51-88, by stage: `8C09ABE8`, 42 bytes), `rk` ranking portraits
(90 + position, `f_8C01C8D0`), `op` prologue (126 + position,
`f_8C01B904`), `win` (162-194 and 267-270, table `8C09B08E`), `bd` win
portraits (195-227, `8C09B116`), `end` endings (228-260, `8C09AC12`: u16, 0
for none). Position: the character number, but `0x13` -> `0x11`, `0x25` ->
`0x15`, and one less above `0x12`.

## Disc and file system

The disc's high-density area starts at LBA 45000 (FAD = LBA + 150). ISO 9660:
volume descriptor at 45016, path tables at 45018 (little-endian) and 45019
(big-endian), root directory at 45020 (11 sectors; a few subdirectories
such as `CAPLOGO`). The game's file system finds directories through the
path table, not only the volume descriptor.

- File system interface: `8C13EE50` (pointer at `8C140618`), a table of
  functions; +60 is open (`f_8C0D824C`, called through `f_8C0D944E(name, 0)`).
- Directory list: `*(*(8C14060C) + 16)`: u32 count, entries from +16, 44
  bytes each: +0 LBA, +4 size, +8 flags (2 = directory), +9 a hash of the
  name (`f_8C0E6CCE`), +10 the name. Looked up linearly (`f_8C0E6E5A`;
  returns index + 1, 0 = not found).
- The game's own loader: `f_8C012C80(name, ...)` queues a name (16 entries
  of 36 bytes from `8C1A34F0`, the name at +4). Load records (120 bytes from
  `8C1A3734`) step through states at +76, handlers at `8C099070`:
  0 `f_8C0132E2` (open: `f_8C013C90` -> the file system; failure ->
  `f_8C013BFC`, the DISC LOAD ERROR path), 1 `f_8C0133D8`, 2 `f_8C01348E`
  (a section), 3 `f_8C01380C`, 4 `f_8C0139DC`, 5 `f_8C013A38`, 6
  `f_8C013AD0`. `f_8C013260` runs them.

## Select screens

Both select screens are drawn from `SELECT.PAC` (loaded at boot): sprite bank
8 (sections `01000008` parts, `02000008` tiles, `04000008` palettes) and
bank B (`0100000B`, `0200000B`; no palettes of its own). Its sections land
in a fixed RAM area with little room between them (bank 8 tiles
`8CAF4800`, palettes `8CB94C00`, parts `8CBA1C00`; bank B tiles
`8CBB4800`, parts `8CBFC800`).

- Bank 8 tiles are 16x16 (128 bytes, 4 bits a pixel, twiddled); a part's
  tiles go row by row. Bank B "tiles" are 32x32: tile n is the four 16x16
  tiles 4n-4n+3 (each compressed on its own, in order top-left,
  bottom-left, top-right, bottom-right).
- Palettes: `f_8C069D64(index, slot, count)` copies bank 8 palettes
  (`*(8C1A20A8)` + index x 32) into the slots. Big portraits use index
  `0x4C0` + character x 6 + colour (`f_8C06BA42`/`BA72`/`BA92`; characters
  0x14 and 0x1C load an extra one, `0x5A4`/`0x5A5`).
- Parts are streamed into per-object textures on first use (`f_8C01A314`)
  and cached by part record (`f_8C01A282`): two objects drawing the same
  record share the first one's texture, even from different tile banks.

Arcade "SELECT PLAYER" screen:

| | |
|---|---|
| cursor grid | `8C0BE3CC`: 7 x 7 bytes, a character per cell; `0x80` ENTRY, `0x40`+ random, `FF` empty |
| faces | a composition (list of 6-byte records: s16 x, s16 y, u16 part; `0x4000` skip, `0x8000` last) at `8C0BF610`, bank B, 24-pixel grid centred on the object; part = face (mostly the character number; 0x15, 0x2A-0x2E others) |
| cell frames | the next list (`8C0BF716`): part 0x27 under each cell |
| ENTRY boxes | another object, bank B parts 0x33-0x38 drawn with bank 8's tiles |
| big portrait | animation per character `8C0C03B4` (8-byte records: duration, flags, -, -, u16 part, u16; flags 0x80 = jump back), bank 8 parts 0x322+ (Ryu) |
| name | `8C0C1194` (bank 8 part per character), scrolling banner `8C0C142C` / `8C0C1478` |
| name strip | down the side for each player: `f_8C06B488(select object, player)` writes the 15 records of `8C0BF1F4` + 90 x side (alternating the side's tag, bank 8 part `0x16D`/`0x16E`, and the name) from `8C0C11F4[character]`: u16 spacing (`0x8000`: the name 8 up), u16 bank 8 part. A part count past the 15 records ran its second loop through 64K records (writing `FF00` from there on) |
| state | the select objects' handlers by state (obj +3): `8C0C10E4`, 7 of them (`f_8C06ABC8`) |

After the pick: mode (NORMAL, MAZI, SAIKYO, CLASSIC), -ism, speed, then
the world map (below).

Versus screen: cursor `f_8C070648`, grid `8C0C1D84` (7 x 6, the sixth row
filled by `f_8C06EFAC` with secret characters in some modes).

Objects pick an animation by kind and character: `8C0BED8C` holds a pointer
per kind (30) to one of two groups of 8-byte records, and the record is
group + 8 x character: `8C0BEB3C` (bank B: the VS screen's portraits, e.g.
Ryu `0x3A`, Dan `0x66`) and `8C0BEC64` (bank 8, from `0x28F`). A record
names a part; the part's record (byte 10 set) says the next part is drawn
with it, so a portrait is a chain (Ryu's four parts `0x3A`-`0x3D`). The
groups have no room after their 38 records (the second ends where the
pointer table starts).

## Arcade mode

- Route: `f_8C02BD08` copies 8 opponents from `8C0A8248[character]` (16
  routes of 8, by game state +0x21 & 15) to `8C189400`, then the rival
  `8C0A7176[character]` and the boss `8C0A719E[character]`; 10 bytes of
  `8C0A82E0` (by game state +0x22) then mark places whose Chun-Li (4) or
  Sodom (6) become `0x12`/`0x14`. Each route's 5th opponent is the
  character's story rival (Ryu: Rose).
- The character's stage: `8C0A8420` (`f_8C02CA78`); per-character flags
  `8C0A8380`, `8C0A83A8`, `8C0A83D0`, `8C0A83F8` (`f_8C02C758` & co.).
- World map: the prologue (`OPnn.PAC`, opened by `f_8C01B904`): portrait,
  personal data, the story; a button turns the page. Then the map with the
  route's ladder (names) and the next stage's place: `8C0C19C8`, (x, y) by
  stage (`f_8C06D8CC`).
- OP file: bank `0x15` (parts 0-3 and 11-14 the screen's frame; the
  portrait from `8C0CF7D4[character]` to the last part), palettes (68 x 32
  bytes), then the text: personal data (10 lines of 18 bytes) and the story
  (4 blocks of 69 bytes, lines of text each starting with position bytes);
  `0x10000A00` the story in Japanese (a 256x256 PVR texture).
- `f_8C069DCC(fighter)` counts a game for the character and -ism: u16
  counters at `8C09B20C` + 8 x character (the system data, saved; the
  ranking follows at `8C09B33C`).

The fight's HUD: `PITU.PAC` (file 5, loaded at boot by `f_8C01BFFA`):
bank `0x10` (sprites: gauges, faces), bank 9 (parts whose tiles are the
16x16 cells of the three 256x256 ARGB1555 textures `0x10000100`-`0x10000102`,
tile n in texture n / 256: the names, digits, banners). `f_8C06443C(hud)`
sets the parts each frame: faces `0x17D` + character (player 1) and `0x1A6`
+ character (player 2, mirrored), names `0xF6` / `0x121` +
`8C0BE266`[character]. The HUD elements are structs with the part at +60
(u16, `0x8000` hidden) and the bank at +54, drawn by `f_8C018BF2`.

Where a file's sections go: per-bank destination tables in the program's
data, parts `8C09AE80`, tiles `8C09AF10`, palettes `8C09AFA0` (a u32 per
bank), copied into `8C1A1F48`/`8C1A1FE8`/`8C1A2088` as files load.

Win screen (`WINnn.PAC`, the `win` table): sections `0x1`/`0x2`/`0x3` the
portrait (parts, tiles, palettes), `0x0` the win quotes (each quote's lines
one after another: position bytes and text, NUL-terminated), `0x40000000`
the sound (`SOSB`). `BDnn.PAC` (`bd` table): the pre-fight dialogue's extra
sprites (bank `0x1A`, not every character has them) and the dialogue in
Japanese (texture `0x10000900`); the English dialogue is in the character
file (descriptor word 38, entry 1: pointers to two-line records).

The PSP (`select_u.pac`, same formats, tiles linear) adds: in the arcade
grid Yun and Ingrid at the ends of row 5, Eagle and Maki in row 6 (where
the Dreamcast has random cells); faces at bank B tiles 44-47 (the ENTRY
boxes moved after them), parts 0x33-0x36; big portraits bank 8 parts
`0x3D7`-`0x416`, palettes `0x4C0` + character x 6 (its palette section has
150 more, so its later indices are shifted: `0x6A1` for the Dreamcast's
`0x60B`).

## Main loop and rendering

`f_8C010424` main loop; tasks `f_8C0259C8` / `f_8C025A90`; the fight's
`f_8C026AD6`. The per-fighter update is `f_8C04729E`. `f_8C0F6E40` waits
for the render to finish (`8C76E808`) with a timeout and resets the
PowerVR on one.

## Option Mode

Menu logic `f_8C07AF3C` (game state +3 cursor, +7/+8 open screen); buttons
are kind-12 objects (`f_8C07D988`, spawn `f_8C025930(kind, id)`).
