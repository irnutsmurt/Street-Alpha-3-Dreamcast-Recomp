#!/usr/bin/env python3
"""Street Fighter Alpha 3 native PC port: setup pipeline.

This repository contains NO game code or data. You supply a dump of your own
Dreamcast disc; this script extracts it, verifies it, statically recompiles the
SH-4 code to C and builds the native executable locally.

    ./sfa3port.py setup path/to/your/disc.gdi      (or .cue / .chd)

Developers:
    ./sfa3port.py rebuild      re-run recompile + CMake build from the extracted disc
    ./sfa3port.py dev-libs     build the prebuilt parts players get (runtime library, CHD helper) with Zig
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools", "recomp"))

import pipeline  # noqa: E402
import zig_build  # noqa: E402

BUILD = os.path.join(ROOT, "build")


def cli_progress():
    last = {"step": None, "pct": -1}

    def show(step, frac, msg):
        pct = int(frac * 100)
        if step != last["step"]:
            if last["step"]:
                print()
            last["step"], last["pct"] = step, -1
            n = pipeline.STEPS.index(step) + 1
            print(f"[{n}/{len(pipeline.STEPS)}] {pipeline.STEP_NAMES[step]}", end="", flush=True)
        if pct != last["pct"] and frac > 0:
            last["pct"] = pct
            print(f"\r[{pipeline.STEPS.index(step) + 1}/{len(pipeline.STEPS)}] {msg} {pct}%   ", end="", flush=True)
    return show


def cmd_setup(args):
    try:
        out = pipeline.run(args.image, force=args.force, toolchain=args.toolchain, progress=cli_progress(),
                           log=lambda m: None, psp_image=args.psp)
    except pipeline.SetupError as e:
        print()
        sys.exit(f"setup failed: {e}")
    print(f"\ndone: {out}")


def cmd_rebuild(args):
    import recompile
    print("[1/2] recompiling SH-4 code to C")
    recompile.run(os.path.join(BUILD, "disc"), os.path.join(BUILD, "generated"))
    print("[2/2] building native executable")
    bdir = os.path.join(BUILD, "cmake")
    subprocess.check_call(["cmake", "-S", ROOT, "-B", bdir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"])
    subprocess.check_call(["cmake", "--build", bdir])
    print(f"done: {os.path.join(bdir, 'sfa3')}")


SDL_SHA256 = "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"  # SDL3-3.4.18.tar.gz (release, signed by Sam Lantinga)


def fetch_sdl3():
    """SDL3's source release, checked against SDL_SHA256. Returns the unpacked folder."""
    import hashlib
    import tarfile
    import urllib.request
    v = zig_build.SDL_VERSION
    work = os.path.join(BUILD, "sdl3")
    src = os.path.join(work, f"SDL3-{v}")
    if os.path.isdir(src):
        return src
    os.makedirs(work, exist_ok=True)
    tgz = src + ".tar.gz"
    if not os.path.exists(tgz):
        print(f"downloading SDL3 {v}")
        urllib.request.urlretrieve(f"https://github.com/libsdl-org/SDL/releases/download/release-{v}/SDL3-{v}.tar.gz", tgz)
    if hashlib.sha256(open(tgz, "rb").read()).hexdigest() != SDL_SHA256:
        sys.exit(f"{tgz}: checksum mismatch")
    with tarfile.open(tgz) as t:
        t.extractall(work, filter="data")
    return src


def vulkan_headers():
    """Vulkan's headers (platform-independent) copied from the system into build/include,
    so that cross-compiling never picks up other Linux headers."""
    import shutil
    inc = os.path.join(BUILD, "include")
    for d in ("vulkan", "vk_video"):
        src, dst = os.path.join("/usr/include", d), os.path.join(inc, d)
        if os.path.isdir(src) and not os.path.isdir(dst):
            shutil.copytree(src, dst)
    if not os.path.exists(os.path.join(inc, "vulkan", "vulkan.h")):
        sys.exit("Vulkan headers not found (install vulkan-headers)")
    return inc


def cmd_dev_libs(args):
    """The prebuilt parts players get, built with Zig for each platform (zig_build.TARGETS):
    build/lib/<platform>/: libsfa3rt.a, SDL3 (libSDL3.so.0 | SDL3.dll + libSDL3.dll.a),
    link/ (what the game links for Vulkan); build/tools/<platform>/chd2gdi"""
    zig = zig_build.find_zig()
    if not zig:
        sys.exit("zig not found (build/toolchain/zig-*/zig or on PATH)")
    sdl_src = fetch_sdl3()
    vk_inc = vulkan_headers()
    bdir = os.path.join(BUILD, "cmake")
    # the shaders' and images' .inc files come from the CMake build (glslc, png2inc)
    subprocess.check_call(["cmake", "-S", ROOT, "-B", bdir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"])
    subprocess.check_call(["cmake", "--build", bdir, "--target", "shaders", "assets"])
    for plat in (["linux", "windows"] if args.platform == "all" else [args.platform]):
        print(f"== {plat}")
        lib_dir = os.path.join(BUILD, "lib", plat)
        sdl_name = "SDL3.dll" if plat == "windows" else "libSDL3.so.0"
        if not os.path.exists(os.path.join(lib_dir, sdl_name)):
            print("SDL3")
            zig_build.build_sdl3(sdl_src, os.path.join(BUILD, "sdl3", plat), lib_dir, zig=zig, platform=plat)
        lib = os.path.join(lib_dir, "libsfa3rt.a")
        zig_build.build_runtime(lib, [os.path.join(bdir, "shaders"), os.path.join(bdir, "assets")],
                                os.path.join(sdl_src, "include"), vk_inc, zig=zig, platform=plat,
                                progress=lambda d, t, l, f: print(f"\rruntime {d}/{t}", end="", flush=True))
        print(f"\n{lib}")
        print(zig_build.vulkan_stub(lib, os.path.join(lib_dir, "link"), zig=zig, platform=plat))
        res = []
        if plat == "windows":  # the UTF-8 manifest, for the game and the helper alike
            res = [zig_build.windows_resources(os.path.join(lib_dir, "sfa3.res"),
                                               os.path.join(ROOT, "assets", "icon.png"), zig=zig)]
            print(res[0])
        L = os.path.join(ROOT, "tools", "third_party", "libchdr")
        helper = os.path.join(BUILD, "tools", plat, "chd2gdi" + zig_build.exe_suffix(plat))
        os.makedirs(os.path.dirname(helper), exist_ok=True)
        subprocess.check_call([zig, "cc", *zig_build.target(plat), "-O2", "-std=gnu11", "-DWANT_RAW_DATA_SECTOR=1",
                               "-DWANT_SUBCODE=1", "-DZSTD_DISABLE_ASM", "-I" + os.path.join(L, "include"),
                               "-I" + os.path.join(L, "deps", "lzma-26.02", "include"),
                               "-I" + os.path.join(L, "deps", "miniz-3.1.2"),
                               "-I" + os.path.join(L, "deps", "zstd-1.5.7"),
                               os.path.join(ROOT, "tools", "chd2gdi.c"), os.path.join(L, "unity.c"),
                               os.path.join(L, "src", "libchdr_codec_avhuff.c"), *res, "-o", helper, "-lm", "-w", "-s"])
        print(helper)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("setup", help="extract, verify, recompile and build from your disc image")
    s.add_argument("image", help="your disc image: .gdi, .cue (Redump CUE/BIN) or .chd")
    s.add_argument("--force", action="store_true", help="skip dump verification")
    s.add_argument("--psp", metavar="IMAGE", help="optional: your PSP Street Fighter Alpha 3 MAX (USA) image (.iso, "
                   ".cso or .zip): adds Eagle, Maki, Yun and Ingrid")
    s.add_argument("--toolchain", choices=["auto", "zig", "cmake"], default="auto",
                   help="auto: the bundled Zig toolchain when the prebuilt runtime is there, else CMake")
    sub.add_parser("rebuild", help="re-run recompile + CMake build from an already extracted disc")
    d = sub.add_parser("dev-libs", help="developers: build the prebuilt libraries and the CHD helper with Zig")
    d.add_argument("--platform", choices=["linux", "windows", "all"], default="all")
    args = ap.parse_args()
    {"setup": cmd_setup, "rebuild": cmd_rebuild, "dev-libs": cmd_dev_libs}[args.cmd](args)


if __name__ == "__main__":
    main()
