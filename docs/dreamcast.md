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
| `8C1A2174` | palette RAM copy (the game uploads it to the PowerVR) |
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
| 5, 7, 9 | tables of pointers (7: per -ism, read at +4 + ism x 4) |
| 12-15 | data tables (pointer tables) |
| 16-27 | code: 18 the per-frame state dispatcher, 19 the round intro, 20 the command check (special moves) |
| 28, 29 | (number, code) lists; 29 (+0x74) patches the engine's jump table `8C0A9278` |
| 30 | points 12 bytes before the descriptor: `[8, code, -1]`, then the descriptor |
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

## Sound

`f_8C059F2C(object, side, sound, ...)`: sound number (high byte: category;
2 = voice, uses the player's bank `8C1A2128`), pan from the object's X less
the camera's. It queues a command (`f_8C014094`) on `8C1A3F9C`; the main
loop's `f_8C01424C` sends them to the sound CPU and waits on some (an entry
reads `FFFF` when done).

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

## Main loop and rendering

`f_8C010424` main loop; tasks `f_8C0259C8` / `f_8C025A90`; the fight's
`f_8C026AD6`. The per-fighter update is `f_8C04729E`. `f_8C0F6E40` waits
for the render to finish (`8C76E808`) with a timeout and resets the
PowerVR on one.

## Option Mode

Menu logic `f_8C07AF3C` (game state +3 cursor, +7/+8 open screen); buttons
are kind-12 objects (`f_8C07D988`, spawn `f_8C025930(kind, id)`).
