#!/usr/bin/env python3
"""The setup pipeline, shared by sfa3port.py (command line) and the converter app.

From the player's own disc image to a playable build, all on their machine:
    1. disc     the image as a GDI (CUE/BIN pointed at, CHD unpacked)
    2. extract  the files of its high-density area
    3. verify   the main executable's SHA-1 (the supported release)
    4. recomp   the SH-4 / ARM code recompiled to C
    5. build    compiled and linked: the bundled Zig toolchain with the
                prebuilt runtime library, or CMake (developers)
    6. finish   build/disc_path.txt (the game reads its disc through it)

Progress goes to a callback: progress(step, fraction, message).
"""
import contextlib
import hashlib
import io
import os
import shutil
import subprocess
import sys

# the source checkout, or the packaged app's folder (PyInstaller: next to the executable)
ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools", "recomp"))

import disc_image  # noqa: E402
import zig_build  # noqa: E402
from version import VERSION  # noqa: E402

STEPS = ["disc", "extract", "verify", "recomp", "build", "finish"]
STEP_NAMES = {
    "disc": "Reading the disc image",
    "extract": "Extracting the game files",
    "verify": "Checking the disc",
    "recomp": "Recompiling the game code",
    "build": "Building the game",
    "finish": "Finishing",
}

# Known-good dumps, identified by the main executable so that any dump format
# of the same release validates identically.
KNOWN = {
    "1b843bbe25e312314ae33a809f52caa8840bf06a": "Street Fighter Alpha 3 (USA) v1.001 T-1203N",
}


class SetupError(Exception):
    pass


def install_info(install):
    """What an earlier conversion recorded in <install>/data/install.json, or None.
    "gdi": the disc it read (an update reuses it), "version": the setup that built it."""
    import json
    try:
        with open(os.path.join(install, "data", "install.json")) as f:
            info = json.load(f)
    except (OSError, ValueError):
        return None
    if not os.path.exists(info.get("gdi", "")):
        info["gdi"] = None  # the disc image was moved or deleted: a full conversion is needed
    return info


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def exe_name(name):
    return name + (".exe" if os.name == "nt" else "")


def find_file(*candidates):
    for c in candidates:
        if c and os.path.exists(c):
            return c
    return None


class Paths:
    """Where things are: a source checkout (build/...) or the packaged app (lib/, bin/, toolchain/).

    install: an install folder for the game (the converter app):
        <install>/sfa3            the game
        <install>/lib/            libraries it needs (bundled SDL3)
        <install>/data/           its data: disc, disc_path.txt, saves, log (and the build's work files)
        <install>/sfa3.cfg        its settings
    Without one, everything stays in the source checkout's build/."""

    def __init__(self, build_dir=None, install=None):
        self.install = install
        self.build = os.path.join(install, "data") if install else (build_dir or os.path.join(ROOT, "build"))
        self.disc = os.path.join(self.build, "disc")
        self.image_work = os.path.join(self.build, "disc_image")
        self.generated = os.path.join(self.build, "generated")
        self.chd_helper = find_file(os.path.join(ROOT, "bin", exe_name("chd2gdi")),
                                    os.path.join(ROOT, "build", "tools", zig_build.HOST, exe_name("chd2gdi")))
        # the prebuilt libraries: lib/ in the packaged app, build/lib/<platform>/ in a checkout
        self.lib_dir = next((d for d in (os.path.join(ROOT, "lib"), os.path.join(ROOT, "build", "lib", zig_build.HOST))
                             if os.path.exists(os.path.join(d, "libsfa3rt.a"))), None)
        self.runtime_lib = self.lib_dir and os.path.join(self.lib_dir, "libsfa3rt.a")
        self.icon = find_file(os.path.join(ROOT, "lib", "icon.png"), os.path.join(ROOT, "assets", "icon.png"))
        self.zig = zig_build.find_zig()
        self.game_dir = install or os.path.join(self.build, "game")


def run(image, force=False, toolchain="auto", progress=None, log=print, paths=None):
    """The whole setup. Returns the path of the built executable.
    image=None updates an existing install (paths.install): the disc it was made
    from is read again, and only what changed is compiled (the object cache)."""
    paths = paths or Paths()
    say = progress or (lambda step, frac, msg: None)

    def step(name, frac=0.0, msg=None):
        say(name, frac, msg or STEP_NAMES[name])

    # 1. the disc as a GDI
    step("disc")
    if image is None:
        info = install_info(paths.install) if paths.install else None
        if not info or not info.get("gdi"):
            raise SetupError("can't update: the disc image this was installed from was moved or deleted. "
                             "Choose your disc image and press Convert (it's quick: the compiled game is "
                             "reused). From then on the install keeps its own copy of the game's data.")
        gdi = info["gdi"]
        source_image = info.get("image")
    else:
        source_image = os.path.abspath(image)
        try:
            gdi = disc_image.to_gdi(image, paths.image_work, paths.chd_helper,
                                    progress=lambda d, t: step("disc", d / max(t, 1), "Unpacking the CHD"))
        except disc_image.ImageError as e:
            raise SetupError(str(e))
    log(f"disc: {gdi}")

    # 2. extract
    step("extract")
    import gdi_extract
    shutil.rmtree(paths.disc, ignore_errors=True)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            listing = gdi_extract.extract(gdi_extract.parse_gdi(gdi), paths.disc)
    except (AssertionError, OSError, ValueError, IndexError) as e:
        raise SetupError(f"could not read the game's files from the disc image ({e})")
    log(f"extract: {len(listing)} files")

    # 3. verify
    step("verify")
    main_exe = os.path.join(paths.disc, "1ST_READ.BIN")
    if not os.path.exists(main_exe):
        raise SetupError("this does not look like a Dreamcast game disc (no 1ST_READ.BIN)")
    h = sha1(main_exe)
    if h in KNOWN:
        log(f"verify: {KNOWN[h]}")
    elif force:
        log(f"verify: unrecognised 1ST_READ.BIN (sha1 {h}), continuing (forced)")
    else:
        raise SetupError("this disc is not the supported release (Street Fighter Alpha 3, USA, v1.001)")

    # the install keeps its own compact copy of the disc (what the game reads while
    # playing): the image it was made from can then be moved or deleted
    if paths.install:
        compact = os.path.join(paths.image_work, "compact")
        if os.path.normcase(os.path.abspath(os.path.dirname(gdi))) != os.path.normcase(os.path.abspath(compact)):
            step("verify", 0, "Copying the game's data into the install")
            tmp = compact + ".new"
            shutil.rmtree(tmp, ignore_errors=True)
            try:
                gdi_extract.write_compact(gdi_extract.parse_gdi(gdi), tmp,
                                          progress=lambda d, t: step("verify", d / max(t, 1),
                                                                     "Copying the game's data into the install"))
            except (OSError, ValueError, IndexError) as e:
                shutil.rmtree(tmp, ignore_errors=True)
                raise SetupError(f"could not copy the game's data into the install ({e})")
            shutil.rmtree(compact, ignore_errors=True)
            os.replace(tmp, compact)
            gdi = os.path.join(compact, "disc.gdi")
            log(f"disc: copied into the install ({gdi})")
        for name in os.listdir(paths.image_work) if os.path.isdir(paths.image_work) else []:  # an unpacked CHD, a CUE's GDI: no longer needed
            if name != "compact":
                p = os.path.join(paths.image_work, name)
                shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)

    # 4. recompile
    step("recomp")
    import recompile
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        recompile.run(paths.disc, paths.generated)
    for line in buf.getvalue().splitlines():
        log("recomp: " + line.strip())

    # 5. build
    use_zig = toolchain == "zig" or (toolchain == "auto" and paths.zig and paths.runtime_lib)
    if use_zig:
        if not paths.zig:
            raise SetupError("the bundled compiler (zig) is missing")
        if not paths.runtime_lib:
            raise SetupError("the prebuilt runtime libraries are missing (developers: ./sfa3port.py dev-libs)")
        os.makedirs(paths.game_dir, exist_ok=True)
        out = os.path.join(paths.game_dir, exe_name("sfa3"))
        # the game may be running: Windows can't overwrite a running .exe but can
        # rename it, so move the old one aside (removed now or by a later update)
        old = out + ".old"
        for leftover in (old,):
            try:
                os.remove(leftover)
            except OSError:
                pass
        if os.path.exists(out):
            try:
                os.replace(out, old)
            except OSError as e:
                raise SetupError(f"can't replace the game ({e.strerror}). Close the game and try again.")
        try:
            zig_build.build_game(paths.generated, paths.runtime_lib, out, zig_build.game_link_args(paths.lib_dir),
                                 zig=paths.zig, cache_dir=os.path.join(paths.build, "cache") if paths.install else None,
                                 log=log,
                                 progress=lambda d, t, label: step("build", d / max(t, 1),
                                                                   "Linking" if label == "link" else
                                                                   f"Compiling ({d} of {t})"))
        except zig_build.BuildError as e:
            if os.path.exists(old) and not os.path.exists(out):
                os.replace(old, out)  # keep the game that was there
            raise SetupError(str(e))
        try:
            os.remove(old)
        except OSError:
            pass  # still running: a later update removes it
    else:
        step("build", 0, "Building (CMake)")
        bdir = os.path.join(paths.build, "cmake")
        try:
            subprocess.check_call(["cmake", "-S", ROOT, "-B", bdir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"])
            subprocess.check_call(["cmake", "--build", bdir])
        except (OSError, subprocess.CalledProcessError) as e:
            raise SetupError(f"the CMake build failed ({e})")
        out = os.path.join(bdir, exe_name("sfa3"))
    log(f"build: {out}")

    # 6. the game finds its disc through build/disc_path.txt
    step("finish")
    if paths.install and use_zig:  # the libraries the game links against, next to it
        if os.name == "nt":  # next to the game, where Windows looks first
            shutil.copy2(os.path.join(paths.lib_dir, "SDL3.dll"), paths.install)
        else:  # found through the game's rpath ($ORIGIN/lib)
            os.makedirs(os.path.join(paths.install, "lib"), exist_ok=True)
            shutil.copy2(os.path.join(paths.lib_dir, "libSDL3.so.0"), os.path.join(paths.install, "lib"))
        if paths.icon:  # for the menu / desktop shortcuts and Steam (Windows uses the one in sfa3.exe)
            shutil.copy2(paths.icon, os.path.join(paths.install, "icon.png"))
        # work files the game doesn't need: the generated C (made again in seconds)
        # and the extracted disc except what it reads from it (the rest comes from
        # the image). The compiled objects stay in data/cache for updates.
        shutil.rmtree(os.path.join(paths.game_dir, "obj"), ignore_errors=True)  # from versions before the cache
        shutil.rmtree(paths.generated, ignore_errors=True)
        for name in os.listdir(paths.disc):
            if name not in ("IP.BIN", "1ST_READ.BIN"):
                p = os.path.join(paths.disc, name)
                shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    with open(os.path.join(paths.build, "disc_path.txt"), "w") as f:
        f.write(gdi + "\n")
    if paths.install:
        import json
        import time
        with open(os.path.join(paths.build, "install.json"), "w") as f:
            json.dump({"version": VERSION, "gdi": os.path.abspath(gdi), "image": source_image,
                       "built": int(time.time())}, f, indent=1)
    step("finish", 1.0, "Done")
    return out
