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
| `8C17FC60` | second game state block (the PSP keeps it apart: `0x8082F8`): +0 the fight's stage (from +0x64, the next one's, as a fight starts: `f_8C025F6E`), +20 the arcade's stage counter (u16) |
| `8C0A0EF4` | stage handlers by stage (42; 0 none), called as a fight starts (`f_8C0218EC`) |
| `8C0A704C` | the last fights' stages by character: row 0 the 9th fight, row 1 (+40) the 10th (`8C02C180`); `0xFF` the opponent's |
| `8C0A8380` | the detour before Bison by character: `0x1E` the dolls, `0x15` Balrog, `0xFF` none (`f_8C02C758`) |
| `8C0B8F74` | intro pairings: a list per character of (opponent, value) bytes ending `FFFF` (`f_8C04E280`) |
| `8C0B1F94` | intro routines (13), by fighter +0x2E3; `8C0B2154` / `8C0B2162` give it from a pairing's value |
| `8C0B9110` | the same pairings for the round's end (`f_8C04E35C`): fighter +0x2E4 from `8C0B2170` / `8C0B2176` |
| `8C0BBA68` | effect animations (85 pointers); sparks (effect type 4) pick one by subtype from `8C0BBC20` |
| `8C0BBBBC` | sparks' handlers by state (4), then by subtype: `8C0BBBCC` the first (21), `8C0BBC58` the second (21) |
| `8C17FC98` | a mode number (s16) indexing per-player tables (`8C09B068`, `8C0BA64C`) |
| `8C17FC9B` | 2: a CPU fighter's files are character `0x15`'s (`f_8C01AEC8`) |
| `8C17FD38` | fighters: 4 x `0x44C` bytes (to `8C180E68`) |
| `8C180E68` | projectiles: 24 x `0x148` bytes (two pools of 12; to `8C182D28`) |
| `8C1832A8` | effect objects: 80 x `0xB0` bytes (pools of 32, 16, 16, 16; to `8C1869A8`) |
| `8C186AB8` | a pool of 3 x `0x30` (allocator `8C0256DC`) |
| `8C1870E8` | menu sprite objects, `0x90` bytes (Option Mode buttons are kind 12) |
| `8C1882E8` | free counters for the object pools (one byte each) |
| `8C1888B4` | camera / scroll block (the main background layer); +0x10 the camera X (16.16); see Stage backgrounds |
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
| +4, +6 (u16) | screen sub-state. In arcade (+2 = 2): +4 1 the map and the wipe to it, 2 the VS screen and the wipe into the fight, 3 the fight (+6: 2 the round's intro, 3 the round, 4 round over, 5 the win pose), 4 the win quote |
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
| +0x140 | score (binary, what the HUD shows; in World Tour the EXP's copy) |
| +0x14E | super meter (0x90 full) |
| +0x155 | CPU-controlled |
| +0x162 | -ism / version byte (also used by the file choice in `f_8C01AEC8`) |
| +0x170, +0x174 | arcade: opponents beaten, bit n for character n (+0x174 from 32; `f_8C02C48C`) |
| +0x180 | the character's descriptor in RAM (39 words, see Descriptor) |
| +0x196 | Final Bison chosen |
| +0x20C / +0x20D | guard meter size / guard damage |
| +0x2D0, +0x2DC | lever input (u16; bits: 1 back, 2 forward, 4 down, 8 up) |
| +0x2E3 | the intro to play (`8C0B1F94`; set by `f_8C04E280` from the pairings `8C0B8F74`) |
| +0x2E7 | pad |
| +0x32C | command input state: 8-byte blocks, one per command |

Projectiles are laid out like the first `0x148` bytes of a fighter. Effect
objects (`0xB0`) share the first part too: +0 in use, +2 type, +3 subtype,
+4 state, +0x48 the fighter it belongs to. `f_8C02570A(pool's last object,
type, subtype)` takes a free one (from the end down); `f_8C02564C(type,
subtype)` in pool A (32 from `8C1832A8`), its siblings the other pools.
Sparks are type 4: `f_8C05ADD0(fighter, opponent, record)` makes one from a
32-byte record (byte 20 the subtype, 15 its sound kind: `8C0BBCAC`), and
`f_8C05ADAE(fighter, n)` with record n of the table at fighter +0xD0.

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
| `0000000A` | the ATTACK LIST's layout: sprite bank `0x1E`'s parts (to `8C7FEE00`), made of the 16x16 cells of the texture `10000E00` |
| `0000000D` | the voice records (64 x 16 bytes) |
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

## CPU players

A CPU fighter runs a script: `f_8C058516(fighter, script)` (the interpreter)
counts fighter +0x1D5 up, keeps the command (u16) at +0x1CA and calls
`8C0B9EBC`[command] (80 commands, `f_8C058536`...) with the script past it.
`f_8C058502` runs the routine `8C0B9EB0`[fighter +0x1C8]. The scripts are
the character's data (in its file); the CPU code is `8C0575xx`-`8C059Exx`.

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
  (`*(8C1A20A8)` + index x 32) into the slots. The sprite beside the big
  portrait (the character standing, the portrait object's animation, bank 8
  parts with palette 0) uses slot 32 + 2 x side, loaded at `8C06BB92` with
  index `0x4C0` + character x 6 + colour (characters 0x14 and 0x1C load an
  extra one into the next slot, `0x5A4`/`0x5A5`). The PSP's are at `0x538` +
  character x 6 + colour (`0x9918C`: after the 120 VS palettes it added),
  and Ingrid (0x29) has two a colour (`0x538` + 6 x 0x29 + 2 x colour,
  count 2).
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

Versus screen (VS MODE, from `SELECT.PAC`): grid `8C0C1D84` (7 x 6; FF
bytes follow it), random cells (`0x40`, `0x41`) at the ends of row 4. Row 5
is the World Tour entries (characters saved from World Tour, slots at
`8C18AFD0` + n x `0x354`): `f_8C06EFAC` (the screen's setup) writes them
into cells 35 + `8C0C1DD8`[n] (columns 0, 1, 2, 4, 5, 6; FF for an empty
slot or in modes 3/13) and their faces (`8C0BE45A`[character], else the
ENTRY box `8C0BE4A6`[n]) into the layout records; `f_8C0703CC` takes a cell
in row 5 (cursor +22) whose column is in `8C0C1DD8` for an entry. The layout
`8C0C0714`: the faces list (rows from the bottom, y = -64 + 24 x row, x =
-88 + 24 x column; the entries' records first, skipped until filled), then
the frames list from record 41 (part `0x27`; the entries' boxes `0x32`-
`0x38`). The cursor `f_8C070648` wraps columns at 6 and rows at 5
(`8C0706E4`), skipping FF cells. `f_8C06F7A4` prints the bottom lines:
"SPEED:" at (120, `0xAE`), "STAGE:" at (114, `0xBE`), their values after.

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
- Pre-fight dialogues: the stage object's +0x42 is the player's script
  (descriptor word 38's entry 0) from `8C0A8468`[character] (`8C0270EA`),
  `8C0A8490`[(object +0x3B - 4) x 40 + character] (`8C0272A0`) or
  `8C0A8508`[the same] (`8C028BD6`), `0xFF` none; `f_8C02CBB6` shows the
  lines (`8C0A8450`: the object's 6 states). The faces: type 14 objects
  with id 6 (`8C06A8BA`), a `SELECT.PAC` bank 8 part from
  `8C0C0E04`[character] with the VS portrait's palette. Bank `0x1A` (`BDnn.PAC`) holds
  extra sprites: tiles `8CA30000`, parts `8CA3E000`. docs/psp-characters.md
  (Pre-fight dialogues) has more.
- The character's stage: `8C0A8420` (`f_8C02CA78`); per-character flags
  `8C0A83A8`, `8C0A83D0`, `8C0A83F8` (`f_8C02C758` & co.).
- The last fights (`f_8C02C758` after each win, by the stage counter, game
  state 2 +20): the 9th and 10th fights' stages from `8C0A704C`
  (`[character]`, `[character + 40]`) into game state 2 +0x64; after the
  9th win a character with a detour in `8C0A8380` (`0x1E` the dolls, Juli
  and Juni; `0x15` Balrog) who hasn't beaten that one yet (fighter
  +0x170/+0x174, `f_8C02C48C`) fights it first, and Bison 11th.
- Intros: `f_8C04E280` looks the opponent up in the pairings (`8C0B8F74`)
  of whichever fighter's +0x155 is 0 and sets both fighters' +0x2E3 from
  `8C0B2154` / `8C0B2162` [value]: an intro routine of `8C0B1F94`.
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
`0x3D7`-`0x416`; its palette section has 150 more (the 4 x 24 VS
palettes and 24 more before the portrait sprites' at `0x538`, Ingrid's 6
extra after them), so its later indices are shifted: `0x6A1` for the
Dreamcast's `0x60B`.

## World Tour

- `WORLD.PAC` (file `0x106`) is bank `0x1D`: the map's and the setup
  screens' sprites. `f_8C01C508` (the map) and `f_8C01C5E8` (the other
  screens) allocate its buffers and load it: `0x4000` bytes for the parts
  (section 4, 345 parts), `0x30000` for the tiles (section 5), `0x400` for
  the palettes (section 6); the sizes are `mov.l` literals (at `8C01C520`
  and `8C01C600` for the tiles).
- `f_8C0366D0` draws the character's banner (name and portrait): three
  parts from `8C0AB7D0`[character] (u16, 38 entries).
- The entries (characters saved from World Tour) are at `8C188D48`
  (2 x `0x354`); the versus select screen shows them in its last row
  (Select screens). An entry: +0 1 in use, +2 the -ism (sign), +4 the total
  level - 1, +5 the X-ism level - 1, +8 EXP, +0x0C / +0x10 / +0x14 the X-,
  A-, V-ism EXP, from +0x2E u16 stats. A fighter in a World Tour fight
  points at its entry (fighter +0x2F0). After a win, `f_8C031CA8` adds the
  fight's EXP (fighter +0x140) to the -ism used (+0x0C + 4 x (+2 + 1), at
  most 9999999) and `f_8C031D40` raises that -ism's level (+5 + index)
  while its EXP passes the -ism's thresholds (u32 tables by level:
  `8C0AAFFC` X, `8C0AB078` A, `8C0AB0F4` V); every level-up also raises the
  total (+4) and the loop stops when the total reaches 31 (level 32), or
  does nothing when it's there already. So the three -isms share 31
  level-ups. `f_8C031DEC` adds the stat gains for each.
- The fight HUD's -ism label (`f_8C0630DC`): part `8C0BE1F4`[style x 2 +
  side + 8 x kind], style 0 / 1 / 2 X / A / V from fighter +0x162's sign, 3
  for a World Tour fighter (its -ism kept at HUD +0x11), kind 0 plain, 1
  World Tour ("Lv"), 2 / 3 MAZI. Style 3 is I-ISM: World Tour fighters show
  "I-ISM Lv", on the disc as in the port (the PSP the same, `0x89CD4`).

## Pause menu: ATTACK LIST

The move list (Training's pause menu, ATTACK LIST) is sprite bank `0x1E`:
its parts are the character archive's section `0xA` (loaded to
`8C7FEE00`), whose tiles are the 16x16 cells of the texture `10000E00`
(cell n at column n % 16, row n / 16; the tile entry's third byte an
attribute: 0x20 text, 0x21-0x23 icons, 0x25 the tabs, 0x26-0x2B the six
colours' markers). Parts 0-5 and 20 are the frame, 6-17 the LEFT / RIGHT
markers, 18-19 the name banner, 21-23 the tabs (MISC., SPECIAL, SUPER),
then a 10x2 part a move: the name and the -ism icons (X, A, V, LV3; cells
8-9, or 6-9) on the top line, the command on the bottom one. Which move
shows on which line comes from a record picked by `8C0D5064`[character]
(`8C08FA32`: record[scroll position x 4 + line] + 18 is the part); nine
shared records `8C0D4EFA`-`8C0D502A`, one per list length. The font is a
white letter (`FBDE`) with a one-pixel shadow (`8842`) right and below,
capitals on rows 2-12.

## Arcade endings

- The arcade's stage counter is `8C17FC97` (0-9; `8C17FC74` follows it);
  after the last stage's win the ending task `f_8C025ECA` runs:
  `f_8C01BA82` loads `endNN.bin` (the template `end00.bin` at `8C095F8C`,
  the character's hex digits put in) to `8C800000`, the overlay slot A, and
  `names[8C09AC12[character]]` (`ENDnn.PAC`); the character is the u16 at
  `8C1A31D4` (`8C17FC92`, with special cases: Bison's and Shin Akuma's
  endings, the second Chun-Li / Sodom / Gen / Balrog files as the first's).
  Then each frame runs the overlay (`f_8C0103FA(8C800000)`); `8C17FCA5` 2+
  runs `f_8C08FDA4` instead.
- Every ending overlay is the same code (to `0xBD08`, 624 functions), then
  the character's data, a directory of it (13 words; its address is the
  code's literal at `0x328`), the overlay's variables (zero in the file) and
  a few library routines (memcpy and the like). The code refers past
  `0xBD08` only to the directory, the variables and the routines (26 literal
  words; `END07` and `END10` also set two words at `0x6ED8`). Directory: 0
  the script; 1 the actors (a list per scene of 32-byte records: byte 0 the
  object type (& `0x7F`; bit 7 the list's last), 1 the sub-type, 3 the
  first animation, 6 the palette base, 31 the character the actor is); 2 /
  4 pointer tables to the animation records (10-byte: duration, flags,
  pose u16 at +4, next s16 at +6) and to 8-byte ones (images); 3 positions;
  5 the palettes; 6-9 the scenes' data; 10 the text records' pointers (the
  count of characters to type, then one or two lines, each an x position
  and its text to a NUL); 11 (texture, palette) pairs to `0xFF` for
  `f_8C0218B6`; 12 the closing screen's logo: its first part in bank
  `0x19` (read as a u16; `END00` `0x14A`, `END06` `0x189`; the same
  pixels in every ending).
- The ending's state is at `8C1A31D4` (+0 character, +2 the scene's event
  index, +4 the scene, +6 a frame count, +`0x3C` the script, `+0x10C`..
  the directory's words 1-10: `+0x110` the 10-byte records' table, `+0x118`
  the 8-byte ones'; another struct from +`0x138`, `8C1A330C`, that much of
  the overlay uses). The setup (`8C800160`) puts its object handlers in the
  effect objects' table: types 72-82 at `8C0A900C + 0x120` (nothing there
  otherwise), copies the directory into the state and calls `f_8C0218B6`.
  Actors are effect objects (pool A) made from the actor records
  (`8C80142A`), bank `0x19` (+`0x46`), the character at +`0x39`.
- Animations: `f_8C024AD4(object, record)` starts one (the record at
  +`0x1C`, its first 4 bytes to +`0x3A`), `f_8C0249E0` the 8-byte kind,
  `f_8C024B1A` steps a frame; the overlay's `8C806DF2` (and copies at
  `8C8087AA`, `8C80919E`) start one from the 10-byte records' table,
  `8C808790` from the 8-byte.
- `ENDnn.PAC`: section 0 and textures `0xC00`-`0xC16` the same in every
  ending, `0x10` (per character), the character's textures from `0xC17`
  (PVR, twiddled: ARGB1555 or 4-bit palettized), `0x900` the text in
  Japanese, sprite bank `0x19` (parts to `8C8E2000`, tiles to `8C812000`:
  over the overlay slots B and C). The Dreamcast's endings carry copies of
  the fighters' sprites (and the effects') in bank `0x19`; the fighters' own
  banks (0 and 1: `8C1A1F48`) are still loaded then.
- Drawing: `f_8C01788E(-, object)` draws a pose (bank +`0x46`'s parts from
  `8C1A1F48`, the pose a run of 12-byte part records to the one whose +10
  is 0); `f_8C018E9C` a part: object +`0x94` set draws the fighters' way
  (32x32 tiles), else through the cached textures (`f_8C01A282` finds one,
  `f_8C01A15E` makes one, `f_8C01A314` streams 16x16 tiles).
- An object's draw mode is +`0x84`: `8C01781C` calls `8C099D38[mode]`
  (0 `f_8C01788E` the animation's pose, 1/2 `8C017900`, 3 `8C0179F2` the
  pose from part +`0x44` (u16; bit 15 set: nothing), 4 `8C017A66`, ...).
- The closing screen: after the scenes the overlay's step `8C800C00` runs
  `8C80A234[+2]`; its first (`8C800C30`) makes object type 76 and waits
  `0x12C` frames. Type 76 (`8C807050`): bank `0x19`, mode 3, part = the
  directory's word 12 (`8C8109B8` in END06's layout), at (`0xC0`, `0x38`),
  palette slot 0 from the overlay's data (`8C80AC64`, `f_8C015AF6`: 16
  colours to `8C1A2174` + 32 x slot); then THANK YOU FOR PLAYING, PRESENTED
  BY and the url (the overlay's strings at `0x98F8`, `0x9910`, `0x9920`)
  by `f_8C01532A`.
- The staff roll's names are in every overlay too (`0x9B9C` on).

## The ranking

- Entries: 16 bytes at `8C09B33C`, 6 lists of `0x220` (34 entries): +0 the
  score (BCD), +4 the name, +8 the character, +13 newly entered. Saved
  with the system data.
- Score Ranking (Mode Menu; the menu modules `8C081xxx`-`8C08Bxxx`): each
  entry's face is a part of the mode menu's sprite bank (`MSEL.PAC`,
  sections 7 parts, 8 tiles, 9 palettes; bank `0x12`), from a byte table by
  character (`8C0D2B01`; copies `8C0D3045`, `8C0D3D1D` for the other
  modules), mirrored by a u16 table (`8C0D2AB2`; copies `8C0D2FF6`,
  `8C0D3CCA`) whose low byte goes to the object's +`0xB`. Drawn by mode 3
  (+`0x44` the part).
- The portrait screen after a ranked game (task `8C061C7E`, steps
  `8C0BE0F0`; step 4 `8C062072`): `f_8C01C8D0` finds the newly entered
  entry, loads `RK_COM.PAC` (bank `0x16`: name plates, part
  `8C0D2035`[character] + 2) and `RKnn.PAC` (bank `0x17`: the portrait from
  part 0, the small sprite's frames, its name strip; palettes), and
  `8C07EAB4` draws them with the animations `8C0D1EA8`[character] and
  `8C0D1F40`[character] (8-byte records, RK parts).
- Quads: `8C0193A0` takes the flips from object +`0x54` (x) and +`0x58`
  (y); the cached textures (`f_8C01A282`) are keyed by the part's tile
  list offset, the bank and the palette.

## Main loop and rendering

`f_8C010424` main loop; tasks `f_8C0259C8` / `f_8C025A90`; the fight's
`f_8C026AD6`. The per-fighter update is `f_8C04729E`. `f_8C0F6E40` waits
for the render to finish (`8C76E808`) with a timeout and resets the
PowerVR on one.

## Stage backgrounds

Units: the game's screen is 384 x 224 units (640 x 448 pixels here, 1 unit
= 1.667 pixels).

Each frame `f_8C021EDC` moves the camera (`f_8C02212E`), the floor's block
(`f_8C0223AA`), layer 1 (`f_8C021F1C`) and layer 3 (`f_8C0221F4`), then
lays the layers out (`f_8C01EBA8`): the polygons are placed now (through
the global screen offset `8C18EF4C` / `8C18EF50`, float pixels) and drawn
at the frame's end.

| Block | |
|---|---|
| `8C188818` | layer 1 (far), `0x9C` bytes |
| `8C1888B4` | the main layer, the camera's |
| `8C188950` | layer 3 |
| `8C1889EC` | the floor's (+0x1C is `8C188A08`) |

A layer block: +2 kind (1: X less `8C188A5C`), +3 its layout (below), +7
set-up step (0 until the layout's first call starts it), +8 its middle X
(16.16; 448), +0x10 X (16.16, +0x12 the whole part), +0x20 the X it moves
to, +0x50 an offset (some stages scroll a layer by itself through it:
backgrounds 14 and 37-40), +0x58 a whole-unit offset, +0x9B which tile
layout. The main block's +0x20 is the camera's X (+0x10 follows it), +0x44
/ +0x46 its limits (the walls: `0x100` and `0x280` on every stage, so the
middle is 448) and +0x7B the layers to lay out: 1 layer 1, 2 the main one,
4 layer 3, 8 the stage's own drawing (below).

The background (`R16(8C17FC60)`, not the stage number at `8C17FCC4`)
indexes `8C0A0C9C` (the stage's own drawing: `f_8C02108C` the floor on
most, `f_8C0212C4`, `f_8C0214AA` 0A's and 1E's), `8C0A131C` (the layers'
start), `8C0A146C` (4 x s16: layer 1's k and its Y's, layer 3's), `8C0A15BC`
/ `8C0A1610` (the floor's).

The floor's block (`8C1889EC`, `f_8C0223AA` each frame): +4 1 while in
use, +0x1C (`8C188A08`, q) = (camera - middle) / +0x80 (16.16; +0x80 from
`8C0A1610`[background]), +0x70 (`8C188A5C`, A) = q x (+0x84 - +0x82) (+0x84
`0xD8`, +0x82 `0xD8` - `8C0A15BC`[background]). Both are 0 with the camera in
the middle and grow with it.

A far layer's X (`f_8C02291C`) is +8 + (camera - middle - A) x k / 16 +
(+0x50), then + (+0x58); backgrounds 4 and 28 use `f_8C022984` (camera -
middle - q x k + ...). So each layer's X is its middle position plus a rate
x (camera - middle), the rate fixed per stage (0A: 0.95 and 0.95; 0C: 0.80
and 0.34; 19: 0.24 and 0.48; 00: 0.20 and 0.61; 0B: 0.14 and 0.56; 06: 1
and 1). The main layer's layout takes a share of the camera off its X too:
`f_8C020804` draws at X - A (stage 00, +3 `0xA0`: X - 8q; stage 04, `0xA4`:
X - q x (+0x84 - +0x82 + 2)), `f_8C01F57C` at X - A for the main layer
(+2 = 1), `f_8C020AD8` (0A, 1E) at X; so the main backdrop moves slower than
the fighters on most stages (0B: 0.56, the same as layer 3: the two are cut
to fit each other). The 4:3 game shows a layer from where it is with the
camera at one wall to where it is at the other, plus a screen: its art, as a
rule; the art behind nearer layers is left unpainted (flat patches of
black, blue, magenta: 0C's sky).

Layouts by +3: the main layer `0xAA` / `0xBE` `f_8C020AD8`, `0xB5` / `0xC0`
`f_8C01F57C` (map +`0x400` / +`0x1400`), else `f_8C020804`; layer 1 `0x7A` /
`0x8E` `f_8C020D08` (a few big textured quads), `0x90` `f_8C020EC0`, else
by +0x9B `f_8C01F57C` (16-unit tiles) or `f_8C01F8C0` (32-unit tiles);
layer 3 `0xE5` / `0xF0` `f_8C01F8C0` (map +`0x400` / +`0x1400`), else as
layer 1. Tile maps: `8C19A424` (layer 1), `8C19C424` (main), `8C19E424`
(layer 3), 1024 units wide (X & `0x3FF`); `f_8C01F57C` / `f_8C01F8C0` stop
at the map's end rather than going round to its start.

Objects on a layer: obj +0x0E (`8C099A48`: 1 `8C188818`, 2 the main
layer, 3 `8C188950`; 0 fixed to the screen). `f_8C01A08E(ctx, obj)` sets
the screen position (ctx +0x6C / +0x6E) to the object's less its layer's,
plus `8C188A08` x obj +0x26 (a depth) on the main layer. Some of the
program's stage objects (effect pool C, `8C1853A8`) are fixed to the screen
and place themselves by the camera: 06's city behind the window, 208 -
0.875 x (camera - 448). `f_8C02472E` draws a stage object only while its
screen X + 64 is 0 to `0x1FF`.

The sprite part drawer (`f_8C018E9C` and three rotated copies) leaves out
the tile columns past the screen's edges (`fcmp` at `8C0191E4` / `8C0191F2`,
`8C019578` / `586`, `8C019994` / `9A2`, `8C019D34` / `D42`: 0 and 640.23).
Each frame also draws thin black strips at the screen's sides (0 - 0.1 and
640 - 640.23 pixels) and borders at the top (0 - 8) and bottom (456 - 480).

Depth: each layer's polygons are at one depth, 1/w = -1 / the float at
`8C0993E8` + (2 x its +0x7A + 1) x 4 (`f_8C01511E`): +0x7A 2 is -650 (the
farthest), 4 -450, 6 -250; the stage's floor and other drawing -500 / -245.
A layer's +0x62 is its scroll Y (units of 2 pixels; it moves a few units as
the camera rises for a jump, each layer by its own amount).

Filler: besides their art, the layers' maps hold parts the 4:3 game never
shows, past where its camera reaches and behind nearer layers. Some is
painted like the rest (more sky, more of a building, 0C's horizon); some is
flat filler in colours the art doesn't use: 0C's sky is navy, black and
magenta (214, 0, 148) behind the houses, 0F's sky ends in a navy column,
05's jungle has flat grey blocks under it, several far layers have flat
strips at their edges. Found by drawing each layer alone and sweeping the
4:3 camera from wall to wall (`runtime/wide_filler.c`; the offline
prototype `build/test/junk.py` found filler on 00, 01, 03, 04, 05, 0A, 0C,
0F, 15, 1A, 1B, 21, 23, 29): hidden one-colour regions of a colour the shown
art doesn't use, that don't continue the art around them; at the very end of
a layer's art, such a region that is a solid rectangle even where its colour
is near the art beside it (08's sky ends in dark-teal bands, 24 units by 148
rows, never shown; 0D's has a flat column, always behind nearer layers); and
next to those, regions of a colour the art does use whose own connected patch
is hidden (0C's black corner: black shows in the 4:3 game's opposite corner),
or at the very end of a layer's art.

16:9 (`runtime/widescreen.c`): the camera is the game's, stopped a margin
(64 units) from each wall; every layer moves at its own speed (from the
stage's tables above); the margins are laid out through the screen offset;
the culls above are widened (`tools/recomp/recompile.py` REPLACE), except
for screen-fixed objects (obj +0x0E 0, the HUD's: the combo counter's banner
starts its slide just past the screen's right edge) other than the stage's
camera-placed ones in effect pool C. Only the fight's own screen (game state
+2 = 2, +4 = 3) is wide. The background extension continues a layer's own
art, blurred, behind what's nearer, over its filler and, past the part of it
the 4:3 game shows (its left edge with the camera at the left wall to its
right edge at the right one), where its art runs out at that part's edge;
the ends of a layer's art that the 4:3 game shows (0B's statue arm, layer 3,
cut to fit the main layer's statue) are its own and stay. The fill is worked
out once a stage from the layer captures (an opaque-list texel captured as
the PowerVR shows it: opaque), in each layer's map (a unit by two pixels:
its texels' size; `wide_extend` in `runtime/wide_filler.c`), and drawn each
frame as a texture (`runtime/shaders/ext.frag`): a gap just behind the layer
(whatever it draws, drawn after, covers it to the pixel; the game draws
fully opaque tiles in the opaque list, those with transparent texels in the
others), with a unit or two of it under the art next to it (the layer is
drawn up to a pixel off the map's grid); filler, grown two units, just in
front; the backdrop's own art behind it too (its layout leaves rows out at
the top when the camera rises for a jump). The backdrop is the farthest layer
laid out; there's none when that one scrolls by itself (+0x50 runs: 20's and
25-28's clouds, also 0E's middle layer), which the game draws all of. Those
layers aren't extended; the fight's own capture leaves them out (their +0x50
isn't 0 by then), the wipe's has them (it is). Shaded as the layer's polygons
are that frame (a super darkens the stage by their colour, `FF606060`). Only
the margins are drawn: the middle of the 16:9 picture is the 4:3 game's own
picture with its camera at the wide view's. Which layers need it is worked
out each frame from their captures, near to far, and each is drawn over only
the columns and rows that need it. Its colours follow the stage's palette
animation (flames, lights, signs): each fill texel keeps the palette of the
art it continues, and is scaled by that palette's colour now against the
capture's.

The fill is worked out on the wipe into the fight (game state +4 = 2), from
the three layers captured on its first frame with them set up (+7), on
threads (about 60-100 ms), a third of a second or more before the round's
first frame; a fight is drawn wide only once its stage's fill is ready (were
it still being worked out then, the fight waits for it; 16:9 picked during a
fight: 4:3 for the few frames it takes). The game sends a frame's polygons to
the TA as the next frame starts and the renderer draws them a frame later, so
each stage layout's state (wide or not, the extensions' positions) goes with
the TA list that holds its polygons (`ta.c`, `widescreen_list_begin`): the
first 16:9 frame of a fight, a hit's screen shake.

A layout steps some stages' animations as it goes: the layer blocks' +0x3C
and +0x83 count down a call on 0A's and 1E's layers 1 and 3 (the storm's
sky and its lightning) and 15's layer 1. A wide fight lays the stage out
seven times a frame (each margin, layer by layer, then the view), and in
0.2.8 those counters stepped three times a frame. Every layout but the
view's own puts the four blocks (`8C188818`, 4 x `0x9C`) back as it found
them (`layout_px`), so they step once a frame as in 4:3. Checked with a
RAM diff around the margins' layouts on all 38 stages: those were the only
lasting writes (the rest: the floor's vertex scratch at `8C1CFE98`, the
stack). The extra layouts' polygons (0A at 16:9: about 2,000 a frame against
850) each looked up their texture, and a paletted one was hashed whole each
time (palette writes aren't tracked): 57 MB a frame on 0A, 10 ms of the
renderer's time, which left no room in the frame and the game ran at half
speed. A texture is checked once a render now (`get_texture`: the guest is
stopped while the renderer runs).

The game puts a quad's corners a hair past whole pixels and its texture
coordinates a hair past whole texels (27.0001; 0.2501 of a 64-texel texture
is 16.0064 at a 16-texel tile's right edge). At 640x480 no pixel centre comes
within that hair of an edge; at a fractional scale (2160 is 4.5 times 480)
whole guest pixels land on render pixel centres, and the centre just inside
a quad's edge took the next tile's texels: a one-pixel line down the quad,
coming and going as a sprite moves (on the right edges of the unmirrored:
player 1). The renderer gives each vertex its polygon's texture coordinate
box (`uv_boxes`), and a point-sampled polygon keeps to the texels it covers
more than an eighth of (`pvr.frag`, `F_TEXEL_BOX`): at whole scales no
sample changes (4:3 against 0.2.6 and 16:9 against 0.2.8 at 1440: the same
pixels). Not fixed by it: a seam in the left margin of 16:9 fights, at any
scale, where the view's own tiles (laid out past the 4:3 screen's left edge)
end over the margin's layout of the same layer, which sits a pixel or two
left of them.

The game is still played on the 4:3 screen: the wide view only draws more of
it, so what the game does "off the screen" can be in the picture. The
screen's left edge is the fighter's layer's +0x12 (`f_8C024698(obj)`:
`8C0A6384[obj +0x0E]`, the main layer for fighters), and "off it" is 64 units
past either edge (X less that, + 64, outside 0 - 0x1FF). Cody's match win
poses 5 and 6 (fighter +0x294, picked from `8C80E1E4` once `8C17FC65` is set;
`f_8C8001A8` and `f_8C800312`, PL1B) run him off the screen
(`f_8C800278`: velocity +0x50, acceleration +0x58, until he's off it; then
the next step and his bit in `8C17FC6B`, a winner's pose done, which on a
CPU win starts the next screen); 5 then keeps him there each frame, running
in place, by the side he faces (+0x0B; `f_8C8002E8`: the left - 64 or the
right + 64), and 6 waits there (until fighter +0x5C is cleared) and runs
back in to the screen's middle (`f_8C80039E`). A policeman (object 27, pool
32 x 0xB0 down from `8C1847F8`, +0x48 Cody) runs in after him from 300 units
off the screen, stops 64 units inside it, then runs off (`f_8C805238`) and is
taken away 80 units past either edge (+4 on to the end, `f_8C8052AE`, which
clears Cody's +0x5C); in 6 a second one comes in from the other side (screen
left + 736 or - 352, `f_8C8052D0`), Cody's punch knocks him away
(`f_8C8053A4`) and he's taken away the same way (`f_8C805410`). At 16:9
those places are in the picture (at a wall, 64 units inside it; the
policeman vanished in view): `hle_cody_run_off`, `hle_cody_kept_off` and
`hle_cody_cop_off` (`recompile.py` OVERLAY_HOOKS: each player slot's copy,
the original passed in) use the picture's edges instead, so they run on
until they're out of it, and a CPU Cody's win moves on a few frames later
than at 4:3.
Other characters' objects taken away 64 units off the screen (Balrog's and
Karin's PL15/PL25/PL1D, the same test) are left as they are.

Stages and their layers' rates, for reference (layer 1 / main / layer 3):
the main layer moves at 1 on 0A and 1E, at the floor's share less on the
rest; layers 1 and 3 from 0.09 (0F) to 1 (06). A far layer of rate r has
art for 384 + 384 r units as the 4:3 game shows it, against the 512 a 16:9
view needs over the same camera range: short on 00, 02, 0B, 0F, 16, 17, 19,
23, 24 (r under 1/3), at the walls on most others.

## Option Mode

Menu logic `f_8C07AF3C` (game state +3 cursor, +7/+8 open screen); buttons
are kind-12 objects (`f_8C07D988`, spawn `f_8C025930(kind, id)`).
