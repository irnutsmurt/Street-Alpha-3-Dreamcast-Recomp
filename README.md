# SFA3 Native — Street Fighter Alpha 3 (Dreamcast) static recompilation

A native PC port (Linux and Windows) of the **US Dreamcast version of Street
Fighter Alpha 3**, built by **statically recompiling** the game's SH-4 machine
code into C and pairing it with a native runtime (Vulkan renderer, SDL3
input/audio). It is not an emulator: no CPU is interpreted or JIT-compiled at
runtime.

> **Built 100% with AI.** All of this project's code, from the recompiler and
> runtime to the setup app and build tooling, was written by AI
> ([Claude Code](https://claude.com/claude-code), Anthropic's Claude), with a
> human directing, testing and play-testing it.

## You need your own copy of the game

**Use your own legally acquired copy of Street Fighter Alpha 3 for the Sega
Dreamcast (USA release).** This project does not include the game, and it
does not tell you where to download it.

Only this release is supported:

| | |
|---|---|
| Title | Street Fighter Alpha 3 (USA), Sega Dreamcast |
| Product number | T-1203N |
| Version | v1.001 |
| Region | NTSC-U (US) |

Other regions (the Japanese *Street Fighter Zero 3*, the European PAL release)
are rejected: the recompiler is tied to this exact build's code. The setup
checks your disc's main executable before doing anything else.

## This repository contains no game data

Nothing from the game ships here: no code, assets or recompiled output. The
setup reads the disc image you made from your own disc, verifies it,
recompiles it and builds the executable on your own machine. Everything
derived from the game stays on your machine (`build/` in a checkout, or the
install folder the setup app creates) and must not be redistributed.

## 1. Dump your disc

Dreamcast GD-ROMs can't be read by ordinary PC drives. Use one of:

- **Dreamcast + SD card adapter**: run a GD-ROM ripping homebrew (e.g. *SD Rip*)
  from a boot disc or ODE, rip to SD card, copy the resulting `.gdi` set to your PC.
- **Dreamcast + Broadband Adapter**: run `httpd-ack` and download the tracks over
  your network (the method documented by Redump).
- **Dreamcast + coder's cable / serial**: dcload-based rippers (slow but works).
- **PC "swap trick"** with a compatible drive (see Redump's GD-ROM dumping guide).

You should end up with a `.gdi` file plus `track01.bin`, `track02.raw`,
`track03.bin`. A Redump CUE/BIN set or a CHD (`chdman createcd`) of it works too.

## 2. Build

### The easy way: the setup app

Download `SFA3-Setup` for your system from the
[Releases](https://github.com/irnutsmurt/Street-Alpha-3-Dreamcast-Recomp/releases)
page, unpack it and run it. Choose your disc
image (GDI, CUE/BIN or CHD) and an install folder, press **Convert**, then
**Play**. It takes about 5 minutes and needs nothing else installed: the app
carries its own C compiler (Zig), SDL3 and the port's runtime. The game needs a
Vulkan graphics driver: on Windows, Windows 10 or 11; on Linux, glibc 2.31 or
newer (Ubuntu 20.04, Debian 11, Fedora 32 or later). `SFA3-Setup --convert IMAGE FOLDER` does the
same without the window.

When it's done, the app has added the game to your Start Menu (Windows) or app
menu (Linux), and to your desktop if you left that ticked. **Add to Steam**
puts it in your Steam library as a non-Steam game, so it works from Big
Picture and on a Steam Deck. The library artwork is downloaded from
[SteamGridDB](https://www.steamgriddb.com/game/1647) at that moment, onto your
machine only (credits below).

### From source (developers)

Requirements: Python 3.9+, CMake, Ninja, a C/C++ compiler (GCC or Clang),
Vulkan SDK/headers + `glslc`, SDL3.

```sh
./sfa3port.py setup "/path/to/Street Fighter Alpha 3.gdi"
```

The pipeline:

1. **Extract** — reads the GD-ROM high-density area's ISO9660 filesystem.
2. **Verify** — checks the SHA-1 of `1ST_READ.BIN` against the known release
   (so any dump format of the same disc validates).
3. **Recompile** — `tools/recomp` disassembles the main executable
   (`1ST_READ.BIN`, loaded at `0x8C010000`) and the 111 per-character code
   overlays (`PLnnA/B/C.BIN`, linked for the player slots at `0x8C800000`,
   `0x8C814000` and `0x8C828000`) and emits C.
4. **Build** — compiles generated C + `runtime/` into `build/cmake/sfa3`.

After changing the recompiler or runtime: `./sfa3port.py rebuild`.

Packaging the setup app (Linux and Windows, both from Linux):

- Zig 0.17 for Linux in `build/toolchain/`, for Windows in `build/toolchain/win/`.
- `./sfa3port.py dev-libs` builds the prebuilt parts players get into
  `build/lib/<platform>/` and `build/tools/<platform>/`: SDL3, the runtime
  library, what the game links for Vulkan, the Windows manifest (UTF-8 paths)
  and the CHD helper. Linux targets glibc 2.31.
- `python3 tools/make_bundle.py` makes the downloads in `build/dist/`. It needs
  PyInstaller in `build/venv`, and for Windows, Windows Python 3.14 with
  PyInstaller in the Wine prefix `build/wine` (`C:\Python314`).

## 3. Play

Run `./build/cmake/sfa3`.

| Keyboard | Dreamcast |
|---|---|
| Arrows / WASD | D-pad |
| Enter | Start |
| U, I, O | X, Y, L trigger (punches) |
| J, K, L | A, B, R trigger (kicks) |
| F1 or Esc | display settings overlay (pauses the game, except in netplay); on the title screen Esc asks QUIT GAME? |
| F11 or Alt+Enter | fullscreen |

Gamepads work too (Back opens the settings menu; on the title screen it asks
QUIT GAME?, YES / NO). Default gamepad layout:
X / Y / RB = light / medium / heavy punch, A / B / RT = light / medium / heavy kick,
LB / LT = all three punches / kicks (Xbox names; same positions on other pads).

**Controls**: *BUTTON CONFIG* (in Option Mode, and in the pause menu during a
fight), one column per player, each set with that player's controller:

- A grid of the attacks, punches on the left and kicks on the right: L, M, H,
  and **3** (all three at once). Each cell shows the key or gamepad button that
  does it (X, RB, TRI, SQR, LSH...); choose a cell and press a new key / button
  to rebind it (Esc / Back cancels). A key or button already in use swaps.
- **VIBRATE**: rumble on the player's gamepad (an emulated Jump Pack).
- **DEVICE**: AUTO, XBOX, PS, SWITCH or KEYBOARD. It picks the device and how
  buttons are named. On AUTO, player 1 uses the first gamepad and the
  keyboard; player 2 the second gamepad.
- **COMMAND** is the game's own: extra time for special-move inputs (ARCADE
  +0, NORMAL +2, LONG +4 frames per step).
- **DEFAULT** also restores that player's bindings.

The same settings are on F1 / Esc > **CONTROLS** (with the keyboard's
directions and START, and the gamepad choice when several are connected).
Gamepads always move with the d-pad and left stick. Player 2 on KEYBOARD
defaults to the numeric keypad, Insert / Home / Page Up and Delete / End /
Page Down. Bindings are saved in `sfa3.cfg` (`p1_hp = Left Shift`,
`p1_pad_3k = lefttrigger`).

**Display settings** — in the game's own menus: *Option Mode > DISPLAY*, and
**DISPLAY** in the pause menu during a fight
(the Dreamcast's CRT "display adjust" screen, rebuilt for PC). The same
settings are also on F1 / Esc at any time (this pauses the game):

| Setting | Values |
|---|---|
| Window mode | window, borderless fullscreen, exclusive fullscreen |
| Window size | 640x480 to 2560x1920 (window mode) |
| Resolution | your monitor's modes and refresh rates (exclusive fullscreen) |
| Picture | 4:3, stretch, integer (pixel-perfect) |
| Render res | internal resolution, 640x480 to 2560x1920 |
| VSync | on / off |
| CRT | off, scanlines, PVM (a Trinitron monitor: beam, mask, glow); level 10-100 |
| Textures | smooth / sharp |
| Skip intro | on: power-on goes straight to the title (Capcom logos, QSound screen and intro skipped; the save still loads) |

Changes apply immediately and are saved to `sfa3.cfg` (not to the memory card
save), which can also be edited by hand.

**Saves** — a memory card is emulated in controller port A, slot 1, and stored
in `build/vmu_a1.bin` (written every few seconds and on exit). Use the game's
Memory Card / Option menus to save and load as on a Dreamcast. The memory card
screen shows one card per controller port, drawn with the port's own images
(`assets/vmu_card.png` for a card, `assets/vmu_empty.png` for none; 64x64,
replaceable, built in when the port is built).

**Netplay** — *Mode Menu > NETWORK* is a two-player lobby (the Dreamcast's
online menu, rebuilt):

- **NAME** sets the name others see (type it, or pick letters with a controller).
- **HOST GAME** waits for a player; it shows your address and port (UDP 7777).
- **JOIN GAME** lists games on your local network; **ENTER ADDRESS** joins by IP.

When a player joins, both games restart into a shared lobby (about two seconds).
The host picks VS Mode, Team Battle, Dramatic Battle or VS Dramatic Battle, and the
game goes straight to that mode's character select. VS Dramatic Battle (two
against one) needs a third player: a second controller on either machine plays
it, otherwise the game's own CPU does, at a level the host sets on that lobby row
(left / right). Players choose their sides as usual; the CPU's teammate picks its
fighter after their own (if the CPU is on its own, whoever finishes choosing first
does); the select screens label it CPU. Quitting a match (pause > Mode Menu) brings both
players back to the lobby; F1 / Esc has **BACK TO LOBBY** for any other screen,
and **LEAVE SESSION**. The host can remove the guest; if either player leaves,
closes the game or loses the connection, the other returns to the NETWORK menu
with a message (the pause menu during a fight also has **LEAVE SESSION**).
Both players use the host's memory card during a session and
nothing is saved; each player's own card is back afterwards.

Command line: `--host`, `--join <address>`, `--name <name>`, `--delay <frames>`
(fixed input delay instead of automatic).

## Layout

| Path | What |
|---|---|
| `sfa3port.py` | setup pipeline entry point |
| `tools/gdi_extract.py` | GDI / ISO9660 extractor |
| `tools/recomp/` | SH-4 decoder, function discovery, C code generator |
| `runtime/` | native runtime: memory, Vulkan PowerVR2 renderer, input, audio, saves |
| `assets/` | the port's own images (not game data) |
| `build/` | **your** extracted disc and all derived output (never commit) |

## License

This project's code is licensed under the **GNU General Public License v3.0
or later** (see [`LICENSE`](LICENSE)).

The licence covers this project's own code and images only. Street Fighter
Alpha 3 is © Capcom; none of its code or data is part of this repository, and
the GPL grants no rights to it.

Third-party components keep their own licences:

| Component | Licence | Use |
|---|---|---|
| [libchdr](https://github.com/rtissera/libchdr) (`tools/third_party/libchdr`) | BSD-3-Clause | the CHD helper |
| [SDL3](https://libsdl.org) | zlib | bundled with the setup app and the game |
| [Zig](https://ziglang.org) | MIT | the setup app's bundled C compiler |

Steam library artwork is not part of this repository or the downloads: **Add
to Steam** fetches it from SteamGridDB on the player's machine. Banner by
Alfonso72394 ([#164774](https://www.steamgriddb.com/grid/164774)), cover by
brendanrfoley ([#297136](https://www.steamgriddb.com/grid/297136)).
