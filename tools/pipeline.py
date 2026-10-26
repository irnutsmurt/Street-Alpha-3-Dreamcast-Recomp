#!/usr/bin/env python3
"""The setup pipeline, shared by sfa3port.py (command line) and the converter app.

From the player's own disc image to a playable build, all on their machine:
    1. disc     the image as a GDI (CUE/BIN pointed at, CHD unpacked)
    2. extract  the files of its high-density area
    3. verify   the main executable's SHA-1 (the supported release)
    4. recomp   the SH-4 / ARM code recompiled to C
    5. addon    optional: the PSP characters (Eagle, Maki, Yun, Ingrid) from the
                player's PSP Street Fighter Alpha 3 MAX image: its code
                recompiled to C, its files converted (tools/psp/)
    6. build    compiled and linked: the bundled Zig toolchain with the
                prebuilt runtime library, or CMake (developers)
    7. finish   build/disc_path.txt (the game reads its disc through it)

Progress goes to a callback: progress(step, fraction, message); skip(step) is
told of a step that had nothing to do.

An update does only what changed. The install records (install.json "stamps")
a fingerprint of each step's inputs: the discs, the code of the tools that ran
(tools/stamps.py), the compiler and the runtime library. A step whose
fingerprint is the same as last time is skipped and its work reused: the PSP
characters' files (<data>/addon), the compiled game code (<data>/cache, with
the list of objects to link) and the game itself. A runtime fix relinks the
game; an app-only release leaves it alone. Within the add-on step, converters
whose code didn't change reuse their output (<data>/addon_cache, cache.py).
run(..., full=True) does everything again (and empties both caches).
When both have to run, the PSP characters convert in a second process while
the game code recompiles.
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
sys.path.append(os.path.join(ROOT, "tools", "psp"))

import disc_image  # noqa: E402
import zig_build  # noqa: E402
from version import VERSION  # noqa: E402

STEPS = ["disc", "extract", "verify", "recomp", "addon", "build", "finish"]
STEP_NAMES = {
    "disc": "Reading the disc image",
    "extract": "Extracting the game files",
    "verify": "Checking the disc",
    "recomp": "Recompiling the game code",
    "addon": "Adding the PSP characters",
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


def build_addon(paths, psp_image, log, step):
    """The PSP characters add-on (tools/psp): from psp_image, or from the install's copy of its files
    (an update). Returns the C files to compile with the game ([] when there's no add-on). The game
    reads the converted files from <data>/addon (named in its files.txt)."""
    import psp_iso
    psp_dir = os.path.join(paths.build, "psp")
    boot = os.path.join(psp_dir, "PSP_GAME", "SYSDIR", "BOOT.BIN")
    data = os.path.join(psp_dir, "PSP_GAME", "USRDIR", "usa", "data")
    if psp_image:
        step("addon", 0, "Reading the PSP disc image")
        try:
            name = psp_iso.extract(psp_image, psp_dir + ".new",
                                   progress=lambda d, t: step("addon", 0.3 * d / max(t, 1), "Reading the PSP disc image"))
        except (psp_iso.ImageError, OSError, ValueError) as e:
            shutil.rmtree(psp_dir + ".new", ignore_errors=True)
            raise SetupError(f"the PSP disc image: {e}")
        shutil.rmtree(psp_dir, ignore_errors=True)
        os.replace(psp_dir + ".new", psp_dir)
        log(f"addon: {name}")
    elif not os.path.exists(boot):
        return []
    import align
    import build_char
    import cache
    import native
    # the converters' work kept between runs (cache.py), for these two discs
    cache.configure(os.path.join(paths.build, "addon_cache"),
                    cache.inputs(os.path.join(paths.disc, "1ST_READ.BIN"), boot))
    log("addon: conversions in " + ("C" if native.LIB else "Python (the C library wasn't found: slower)"))
    maps = os.path.join(paths.build, "psp_maps.json")
    out_dir = os.path.join(paths.build, "psp_out")
    step("addon", 0.35, "Lining up the PSP's code with the Dreamcast's")

    def line_up():
        with contextlib.redirect_stdout(io.StringIO()):
            align.write_maps(paths.disc, boot, maps)
        return open(maps).read()
    text = cache.value("maps", ["align"], [], line_up)
    with open(maps, "w") as f:
        f.write(text)
    step("addon", 0.45, "Converting the PSP characters")
    shutil.rmtree(out_dir, ignore_errors=True)
    out = io.StringIO()
    try:
        with contextlib.redirect_stdout(out):
            problems = build_char.build(boot, data, paths.disc, out_dir, maps)
    except (AssertionError, OSError, ValueError, KeyError, IndexError, SystemExit) as e:
        raise SetupError(f"converting the PSP characters failed ({e})")
    cache.finish()
    for line in out.getvalue().splitlines():
        log("addon: " + line)
    if problems:
        raise SetupError(f"converting the PSP characters left {len(problems)} problems (first: {problems[0]})")
    addon_dir = os.path.join(paths.build, "addon")
    shutil.rmtree(addon_dir, ignore_errors=True)
    os.makedirs(addon_dir)
    names = sorted(n for n in os.listdir(out_dir) if n.upper().endswith((".PAC", ".BIN")))
    for n in names:
        shutil.copy2(os.path.join(out_dir, n), os.path.join(addon_dir, n))
    with open(os.path.join(addon_dir, "files.txt"), "w") as f:
        f.write("".join(n + "\n" for n in names))
    step("addon", 1.0, "PSP characters converted")
    return [os.path.join(out_dir, n) for n in sorted(os.listdir(out_dir)) if n.startswith("psp_") and n.endswith(".c")]


def _addon_child(paths, psp_image, q):
    """build_addon in a second process: its log lines, progress and result through q"""
    try:
        psp_c = build_addon(paths, psp_image, lambda m: q.put(("log", m)),
                            lambda name, frac=0.0, msg=None: q.put(("step", name, frac, msg)))
        q.put(("done", psp_c))
    except SetupError as e:
        q.put(("error", str(e)))
    except Exception as e:  # anything else: still reported
        q.put(("error", f"converting the PSP characters failed ({e!r})"))


class AddonProcess:
    """the PSP characters converting in a second process (multiprocessing, spawned: the same on
    Linux and Windows, and in the packaged app, whose main calls freeze_support())"""

    def __init__(self, paths, psp_image):
        import multiprocessing
        ctx = multiprocessing.get_context("spawn")
        self.q = ctx.Queue()
        self.p = ctx.Process(target=_addon_child, args=(paths, psp_image, self.q), daemon=True)
        self.p.start()

    def wait(self, step, log):
        """forward what it reported, until it's done: its C files"""
        import queue
        while True:
            try:
                ev = self.q.get(timeout=0.5)
            except queue.Empty:
                if not self.p.is_alive() and self.q.empty():
                    raise SetupError(f"converting the PSP characters failed (the process ended, {self.p.exitcode})")
                continue
            if ev[0] == "log":
                log(ev[1])
            elif ev[0] == "step":
                step(*ev[1:])
            elif ev[0] == "done":
                self.p.join(10)
                return ev[1]
            else:
                self.p.join(10)
                raise SetupError(ev[1])

    def stop(self):
        if self.p.is_alive():
            self.p.terminate()


def _stamps(paths, disc_sha1, has_addon, use_zig):
    """the fingerprints of each step's inputs this time (None: unknown, never matching)"""
    import stamps
    psp_boot = os.path.join(paths.build, "psp", "PSP_GAME", "SYSDIR", "BOOT.BIN")
    out = {"recomp": stamps.key("recomp", disc_sha1, stamps.closure("recompile"))}
    if has_addon:
        import native
        out["addon"] = stamps.key("addon", disc_sha1, sha1(psp_boot) if os.path.exists(psp_boot) else None,
                                  stamps.closure("align", "build_char", "psp_iso", "cache"), native.VERSION)
    if use_zig and paths.lib_dir:
        out["objects"] = stamps.key("objects", out["recomp"], out.get("addon", "no add-on"),
                                    zig_build.objects_fingerprint(paths.zig))
        out["game"] = stamps.key("game", out["objects"],
                                 zig_build.link_fingerprint(paths.runtime_lib, zig_build.game_link_args(paths.lib_dir),
                                                            paths.zig))
    return out


def run(image, force=False, toolchain="auto", progress=None, log=print, paths=None, psp_image=None, full=False,
        skip=None):
    """The whole setup. Returns the path of the built executable.
    image=None updates an existing install (paths.install): the disc it was made
    from is read again, and only what changed is compiled (the object cache).
    psp_image: the player's PSP Street Fighter Alpha 3 MAX (USA) image for the PSP characters add-on
    (.iso, .cso or a .zip of one); an install that has the add-on keeps a copy of its files and updates
    it without the image."""
    paths = paths or Paths()
    say = progress or (lambda step, frac, msg: None)
    skipped = skip or (lambda step: None)

    def step(name, frac=0.0, msg=None):
        say(name, frac, msg or STEP_NAMES[name])

    def unchanged(name, what):
        log(f"{name}: unchanged since the last build: {what} reused")
        skipped(name)

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
    import gdi_extract

    def extract():
        step("extract")
        shutil.rmtree(paths.disc, ignore_errors=True)
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                listing = gdi_extract.extract(gdi_extract.parse_gdi(gdi), paths.disc)
        except (AssertionError, OSError, ValueError, IndexError) as e:
            raise SetupError(f"could not read the game's files from the disc image ({e})")
        log(f"extract: {len(listing)} files")

    # what the last build recorded (an update of an install: the same disc, the install's own copy)
    info = install_info(paths.install) if paths.install and image is None else None
    previous = {} if full or not info else dict(info.get("stamps") or {})
    if full and paths.install:  # everything again: the caches emptied too
        for d in ("cache", "addon_cache"):
            shutil.rmtree(os.path.join(paths.build, d), ignore_errors=True)
    main_exe = os.path.join(paths.disc, "1ST_READ.BIN")
    extracted = False
    if not previous or not os.path.exists(main_exe):
        extract()  # 2. extract
        extracted = True

    # 3. verify
    step("verify")
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

    # what this build's steps depend on, against what the last one recorded
    use_zig = toolchain == "zig" or (toolchain == "auto" and paths.zig and paths.runtime_lib)
    psp_boot = os.path.join(paths.build, "psp", "PSP_GAME", "SYSDIR", "BOOT.BIN")
    has_addon = bool(psp_image) or os.path.exists(psp_boot)
    now = _stamps(paths, h, has_addon, use_zig) if paths.install else {}
    if psp_image:  # a PSP image given: converted again (and compiled: it may be another dump)
        now["addon"] = now["objects"] = now["game"] = None
    same = {k for k, v in now.items() if v is not None and previous.get(k) == v}
    addon_dir = os.path.join(paths.build, "addon")
    objects = zig_build.cached_objects(os.path.join(paths.build, "cache")) if use_zig and paths.install else None
    exe = os.path.join(paths.game_dir, exe_name("sfa3"))
    need_objects = not ({"objects"} <= same and objects)
    need_recomp = need_objects or not use_zig
    need_addon = has_addon and (need_objects or "addon" not in same or not os.path.exists(addon_dir))
    need_link = need_objects or "game" not in same or not os.path.exists(exe)
    if (need_recomp or need_addon) and not extracted:
        extract()  # the recompiler and the add-on read the disc's other files
    elif not extracted:
        unchanged("extract", "the disc's files")

    # 4. recompile, and 5. the PSP characters (optional), side by side when both run
    psp_c = []
    child = None
    if need_recomp and need_addon:
        try:
            child = AddonProcess(paths, psp_image)
        except (OSError, ValueError, ImportError) as e:  # no second process: one after the other
            log(f"addon: not in a second process ({e})")
    if need_recomp:
        step("recomp")
        import recompile
        try:
            recompile.run(paths.disc, paths.generated, report=lambda m: log("recomp: " + m),
                          progress=lambda d, t: step("recomp", d / max(t, 1)))
        except BaseException:
            if child:
                child.stop()
            raise
    else:
        unchanged("recomp", "the compiled game code")
    if need_addon:
        psp_c = child.wait(step, log) if child else build_addon(paths, psp_image, log, step)
    elif has_addon:
        unchanged("addon", "the PSP characters' files")
    else:
        unchanged("addon", "nothing (no PSP characters)")
    if not has_addon:
        shutil.rmtree(addon_dir, ignore_errors=True)

    # 6. build
    if use_zig and psp_c:  # the add-on's C goes with the game's (CMake finds build/psp_out itself)
        for src in psp_c:
            shutil.copy2(src, os.path.join(paths.generated, os.path.basename(src)))
        with open(os.path.join(paths.generated, "sources.txt"), "a") as f:
            f.write("".join(os.path.basename(src) + "\n" for src in psp_c))
    if use_zig and not need_link:
        unchanged("build", "the game")
        out = exe
    elif use_zig:
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
            if need_objects:
                zig_build.build_game(paths.generated, paths.runtime_lib, out, zig_build.game_link_args(paths.lib_dir),
                                     zig=paths.zig,
                                     cache_dir=os.path.join(paths.build, "cache") if paths.install else None,
                                     log=lambda m: log("build: " + m),
                                     progress=lambda frac, msg: step("build", frac, msg))
            else:  # the game code is as it was: only linked again (a new runtime library)
                log("build: the compiled game code unchanged: linked again")
                zig_build.link_game(objects, paths.runtime_lib, out, zig_build.game_link_args(paths.lib_dir),
                                    zig=paths.zig, log=lambda m: log("build: " + m),
                                    progress=lambda frac, msg: step("build", frac, msg),
                                    timings_path=os.path.join(paths.build, "cache", "timings.json"))
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

    # 7. the game finds its disc through build/disc_path.txt
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
        shutil.rmtree(os.path.join(paths.build, "psp_out"), ignore_errors=True)  # the add-on's: made again each time
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
                       "psp_characters": has_addon, "built": int(time.time()),
                       "stamps": {k: v for k, v in now.items() if v} if use_zig else {}}, f, indent=1)
    step("finish", 1.0, "Done")
    return out
