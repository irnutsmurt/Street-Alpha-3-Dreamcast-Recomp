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

Status (2026-10-24):

- PSP Ryu runs on recompiled PSP code in Ryu's place, all specials and
  supers.
- Eagle plays in Ryu's place: intro, walking, normals, specials and supers.
  His voices are muted for now (`SFA3_PSP_MUTE`): the PSP's sound numbers
  stall the Dreamcast's sound queue until the sound banks are converted.
- Maki, Yun and Ingrid build; Yun's code still has PSP addresses with no
  Dreamcast counterpart (see Gaps).
- Characters `0x26`-`0x29` as new numbers: Eagle loads and fights as
  `0x26` (forced with `SFA3_FORCE_CHAR`, files added with `SFA3_ADD`). The
  HUD shows Ryu's name and portrait: the tables indexed by character number
  have no entries for the new numbers yet.

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
4. Writes the recompiled code: `psp_code_<n>.c` and `psp_tables.c` (the
   function table, regions, code map), which the runtime build picks up from
   `build/psp_out`.

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
| `runtime/psp_load.c` | loading: big tile sections, the new characters' file names |
| `runtime/psp_none.c` | empty tables when no PSP character is built |
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
there on a real Dreamcast), served by `mmio.c` from `g_ext_ram`. Used for:

- tile sections too big for a slot: `hle_pac_section` (wraps `f_8C01348E`)
  sends a 0x200000 section that won't fit, with `0x800` bytes kept for the
  palettes, to `98000000 + slot x 0x400000` and points the per-slot and
  per-bank tile tables there;
- the new characters' archive names (`98F00000`).

### New character numbers

- Archive: `names[13 + character]` would read past the name table, so
  `hle_open_file` (wraps `f_8C012C80`) gives `pl26.pac`...`pl29.pac` when
  the call comes from `f_8C01B598` (return address `8C01B648`).
- Overlay: the template already makes `PL26A.BIN` etc.
- Disc: the files aren't on the disc. `SFA3_ADD=NAME=path;...` adds files:
  `gdi.c` serves a rebuilt root directory at LBA `0x7F0000` and the files'
  data from LBA `0x800000` up. Everything that says where the root is gets
  patched as it's read: the volume descriptor, both path tables (the game's
  file system finds the root through them; missing this gave DISC LOAD
  ERROR) and the subdirectories' `..` records. `SFA3_REPLACE=NAME=path;...` swaps an existing file's
  contents.
- Still to do: the 42 tables the Dreamcast indexes by character number
  (`tools/psp/dc_idtables.py`, list below) need entries for `0x26`-`0x29`; the
  recompiler can move a table (`relocate.json`: literal pool addresses
  rewritten) to make room.

Dreamcast tables indexed by character number:

```
8C09B116 8C0A7018 8C0A7176 8C0A719E 8C0A8248 8C0A8380 8C0A83A8 8C0A83D0
8C0A83F8 8C0A879A 8C0AB820 8C0B2088 8C0B2254 8C0B57B8 8C0B57DE 8C0B7C08
8C0B7CC0 8C0B7D58 8C0B8DA8 8C0B8DCE 8C0B8DF4 8C0B8E1A 8C0B8F74 8C0B9A74
8C0BA66C 8C0BBD44 8C0BC0BC 8C0BC5D8 8C0BD570 8C0BE266 8C0C0E54 8C0C111C
8C0C1350 8C0C142C 8C0C1478 8C0C15A0 8C0C1AC8 8C0C1B14 8C0D2354 8C0D5064
8C0D5498 8C1D1608
```

The PSP's (64 candidates, `build/test/idtables.py`) include `0x2173F8`,
`0x2174A0`, `0x2174F4`, `0x2178C8`; Yun's data points into
`0x215898`-`0x2179D0` (read by `0x642AC`, `0x64368`).

## Gaps

- Yun: PSP `0x39CC70` has no Dreamcast counterpart found yet; his engine
  table and function pointers need theirs.
- Sound: PPHD/VAG -> SOSB/SMPB, then drop `SFA3_PSP_MUTE`.
- Select screen (the PSP's layout and portraits), names, portraits and VS
  screens (the `0x10000E00` textures), win quotes, endings (`end26`-`end29`),
  CPU behaviour, stages and music.

## Debugging switches (environment)

| Variable | What |
|---|---|
| `SFA3_REPLACE=NAME=path;...` | serve a file's contents from a host file |
| `SFA3_ADD=NAME=path;...` | add files to the disc |
| `SFA3_FORCE_CHAR=player:hex` | write the character number every frame |
| `SFA3_PSP_MUTE=1` | skip the PSP code's sound calls |
| `SFA3_PSP_TRACE=addr` | trace one PSP function (`0`: all, from `SFA3_WATCH_FROM`) |
| `SFA3_PSP_STUBS=frame` | log stub calls from that frame |
| `SFA3_FILE_LOG=1` | log files opened by name, with the caller |
| `SFA3_RENDER_LOG=frame` | log the render from that frame |
| `SFA3_WATCH_WRITE=addr,len` | log writes to guest memory (with the writing function) |
| `SFA3_WATCH_FROM=frame` | start watching or tracing at that frame |
| `SFA3_DMA_LOG=1` | log DMA |

## Test scripts (`build/test`, not in the repository)

`run.sh` (instance 7), `id_fight.sh <id>` (character `<id>` forced for
player 1, files added with `SFA3_ADD`, Versus against M.Bison),
`eagle_fight.sh`, `motion.sh` (inputs: qcf, qcb, dp, rdp, hcf, hcb, qcf2,
qcb2, mash), `movestrip.sh`, `shots.sh`, `alive.sh`, `mkpac.py`
(a PSP archive in the Dreamcast's form), `idtables.py`, `desccmp.py`,
`newchars.py`.
