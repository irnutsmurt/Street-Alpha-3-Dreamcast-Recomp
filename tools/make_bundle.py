#!/usr/bin/env python3
"""Developers: assemble the converter app's download, for Linux or Windows (x86_64).

    python3 tools/make_bundle.py [--platform linux|windows|all]   (after ./sfa3port.py dev-libs)

Makes build/dist/SFA3-Setup-<platform>-x86_64/ and a .tar.xz (Linux) / .zip (Windows) of it:
    SFA3-Setup[.exe]        the app (PyInstaller, with its Python inside)
    _internal/              the app's Python runtime and modules
    toolchain/zig-*/        the C compiler (Zig), trimmed to the target used
    lib/libsfa3rt.a         the port's runtime, prebuilt (our code)
    lib/                    SDL3 (installed next to the game) and what the game links against
    runtime/*.h             the runtime's headers (the recompiled C includes them)
    bin/chd2gdi[.exe]       the CHD helper (libchdr)
    licenses/               third-party licences
Nothing from the game is in it: players convert their own disc.

The Windows app is made by Windows Python + PyInstaller running under Wine, in the
prefix build/wine (C:\\Python314); Windows Zig is in build/toolchain/win/.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import zig_build  # noqa: E402

DIST = os.path.join(ROOT, "build", "dist")
ZIG_VERSION = "0.17.0"

# per platform: Zig's C headers and libc sources to keep (the rest is other targets)
KEEP = {
    "linux": {"include": {"any-linux-any", "generic-glibc", "x86-linux-gnu", "x86-linux-any"}, "libc": {"glibc", "include"}},
    "windows": {"include": {"any-windows-any"}, "libc": {"mingw", "include"}},
}
ZIG_DROP = ("doc", "docs", "lldb", "libcxx", "libcxxabi", "libtsan")


def need(path, hint="run ./sfa3port.py dev-libs"):
    if not os.path.exists(path):
        sys.exit(f"missing {path} ({hint})")
    return path


def needed(path):
    """an ELF file's DT_NEEDED libraries"""
    r = subprocess.run(["readelf", "-d", path], capture_output=True, text=True)
    return re.findall(r"\(NEEDED\).*\[(.+?)\]", r.stdout)


def prune_internal(out):
    """Linux: PyInstaller picks up the system's Tcl packages (Graphviz) and, through them,
    image and video codec libraries. Drop those, then every top-level library that
    nothing left links against."""
    internal = os.path.join(out, "_internal")
    for d in ("graphviz",):
        shutil.rmtree(os.path.join(internal, "_tcl_data", d), ignore_errors=True)
    top = {f for f in os.listdir(internal) if ".so" in f and os.path.isfile(os.path.join(internal, f))}
    roots = [os.path.join(out, "SFA3-Setup")]
    for d, _, fs in os.walk(internal):
        for f in fs:
            if d != internal and ".so" in f:
                roots.append(os.path.join(d, f))  # extension modules, Tcl/Tk packages
    core = {f for f in top if f.startswith(("libpython", "libtcl", "libtk"))}
    roots += [os.path.join(internal, f) for f in core]
    keep, todo = set(core), roots
    while todo:
        for n in needed(todo.pop()):
            if n in top and n not in keep:
                keep.add(n)
                todo.append(os.path.join(internal, n))
    for f in top - keep:
        os.remove(os.path.join(internal, f))
    print(f"  pruned {len(top - keep)} unused libraries")


def winpath(p):
    """a Linux path as Wine's Z: drive sees it"""
    return "Z:" + os.path.abspath(p).replace("/", "\\")


# the PSP characters add-on's tools (tools/psp; build_char imports most of them inside functions)
PSP_MODULES = ["psp_iso", "align", "build_char", "chartables", "screens", "ranking", "menus", "hud", "worldtour",
               "files", "ending", "throws", "arcade", "sound", "sprites", "pac", "mipsgen", "mips"]


def pyinstaller(platform, dist):
    args = ["--noconfirm", "--onedir", "--windowed", "--name", "SFA3-Setup"]
    work = os.path.join(ROOT, "build", "pyinstaller", platform)
    paths = {"dist": dist, "work": work, "spec": work, "tools": os.path.join(ROOT, "tools"),
             "recomp": os.path.join(ROOT, "tools", "recomp"),
             "psp": os.path.join(ROOT, "tools", "psp"),
             "extra": os.path.join(ROOT, "tools", "recomp", "extra_entries.txt"),
             "maps": os.path.join(ROOT, "tools", "psp", "manual_maps.json"),
             "app": os.path.join(ROOT, "converter.py")}
    if platform == "windows":
        paths = {k: winpath(v) for k, v in paths.items()}
        env = dict(os.environ, WINEPREFIX=os.path.join(ROOT, "build", "wine"), WINEDEBUG="-all")
        cmd = ["wine", "C:\\Python314\\python.exe", "-m", "PyInstaller"]
        sep = ";"
    else:
        env = None
        cmd = [need(os.path.join(ROOT, "build", "venv", "bin", "pyinstaller"),
                    "python3 -m venv build/venv && build/venv/bin/pip install pyinstaller")]
        sep = ":"
    subprocess.check_call(cmd + args + [
        "--distpath", paths["dist"], "--workpath", paths["work"], "--specpath", paths["spec"],
        "--paths", paths["tools"], "--paths", paths["recomp"], "--paths", paths["psp"],
        "--hidden-import", "recompile", "--hidden-import", "gdi_extract",
        *[a for m in PSP_MODULES for a in ("--hidden-import", m)],
        "--add-data", paths["extra"] + sep + ".", "--add-data", paths["maps"] + sep + ".", paths["app"]], env=env)


def copy_zig(platform, out):
    src = (os.path.join(ROOT, "build", "toolchain", "win", f"zig-x86_64-windows-{ZIG_VERSION}") if platform == "windows"
           else os.path.join(ROOT, "build", "toolchain", f"zig-x86_64-linux-{ZIG_VERSION}"))
    need(src, f"unpack Zig {ZIG_VERSION} for {platform} there")
    dst = os.path.join(out, "toolchain", os.path.basename(src))
    shutil.copytree(src, dst, symlinks=True, ignore=shutil.ignore_patterns(*ZIG_DROP))
    libc = os.path.join(dst, "lib", "libc")
    for d in os.listdir(libc):
        if d not in KEEP[platform]["libc"] and os.path.isdir(os.path.join(libc, d)):
            shutil.rmtree(os.path.join(libc, d))
    inc = os.path.join(libc, "include")
    for d in os.listdir(inc):
        if d not in KEEP[platform]["include"]:
            shutil.rmtree(os.path.join(inc, d))
    return src


def bundle(platform):
    name = f"SFA3-Setup-{platform}-x86_64"
    out = os.path.join(DIST, name)
    lib_dir = os.path.join(ROOT, "build", "lib", platform)
    libs = (["libsfa3rt.a", "SDL3.dll", "libSDL3.dll.a", "sfa3.res", "link/libvulkan-1.a"] if platform == "windows"
            else ["libsfa3rt.a", "libSDL3.so.0", "link/libvulkan.so.1"])
    for f in libs:
        need(os.path.join(lib_dir, f))
    helper = need(os.path.join(ROOT, "build", "tools", platform, "chd2gdi" + zig_build.exe_suffix(platform)))

    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(DIST, exist_ok=True)
    print(f"== {platform}: app (PyInstaller)")
    tmp = os.path.join(DIST, "pyi-" + platform)
    shutil.rmtree(tmp, ignore_errors=True)
    pyinstaller(platform, tmp)
    os.rename(os.path.join(tmp, "SFA3-Setup"), out)
    shutil.rmtree(tmp)
    if platform == "linux":
        prune_internal(out)

    print("toolchain (Zig, trimmed)")
    zsrc = copy_zig(platform, out)

    print("runtime, helper, licences")
    for f in libs:
        os.makedirs(os.path.dirname(os.path.join(out, "lib", f)), exist_ok=True)
        shutil.copy2(os.path.join(lib_dir, f), os.path.join(out, "lib", f))
    shutil.copy2(os.path.join(ROOT, "assets", "icon.png"), os.path.join(out, "lib", "icon.png"))
    os.makedirs(os.path.join(out, "runtime"))
    for h in glob.glob(os.path.join(ROOT, "runtime", "*.h")):
        shutil.copy2(h, os.path.join(out, "runtime"))
    os.makedirs(os.path.join(out, "bin"))
    shutil.copy2(helper, os.path.join(out, "bin"))
    lic = os.path.join(out, "licenses")
    os.makedirs(lic)
    shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(out, "LICENSE.txt"))  # this project: GPL-3.0-or-later
    shutil.copy2(os.path.join(ROOT, "tools", "third_party", "libchdr", "LICENSE.txt"), os.path.join(lic, "libchdr.txt"))
    shutil.copy2(os.path.join(zsrc, "LICENSE"), os.path.join(lic, "zig.txt"))
    shutil.copy2(os.path.join(ROOT, "build", "sdl3", f"SDL3-{zig_build.SDL_VERSION}", "LICENSE.txt"),
                 os.path.join(lic, "SDL3.txt"))
    needs = ("Windows 10 or 11 and a graphics driver with Vulkan (any recent AMD, NVIDIA or Intel driver)."
             if platform == "windows" else
             "a Vulkan-capable graphics driver and glibc 2.31 or newer\n"
             "(Ubuntu 20.04, Debian 11, Fedora 32 or anything more recent).")
    with open(os.path.join(out, "README.txt"), "w", newline="\r\n" if platform == "windows" else "\n") as f:
        f.write("Street Fighter Alpha 3 - native PC port setup\n\n"
                f"Run SFA3-Setup{zig_build.exe_suffix(platform)}, choose your own disc image (GDI, CUE/BIN or CHD of\n"
                "the USA release) and an install folder, press Convert, then Play.\n\n"
                "Nothing from the game is included: the game is rebuilt from your disc on your machine.\n"
                f"The game needs {needs}\n")

    print("archive")
    if platform == "windows":
        arc = out + ".zip"
        with zipfile.ZipFile(arc, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for d, _, fs in os.walk(out):
                for f in fs:
                    p = os.path.join(d, f)
                    z.write(p, os.path.join(name, os.path.relpath(p, out)))
    else:
        arc = out + ".tar.xz"
        with tarfile.open(arc, "w:xz", preset=6) as t:
            t.add(out, arcname=name)
    size = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(out) for f in fs)
    print(f"{out} ({size / 2**20:.0f} MB)\n{arc} ({os.path.getsize(arc) / 2**20:.0f} MB)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platform", choices=["linux", "windows", "all"], default="all")
    a = ap.parse_args()
    for p in (["linux", "windows"] if a.platform == "all" else [a.platform]):
        bundle(p)


if __name__ == "__main__":
    main()
