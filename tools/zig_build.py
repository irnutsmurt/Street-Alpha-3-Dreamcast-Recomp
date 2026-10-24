#!/usr/bin/env python3
"""Building the port with the bundled Zig toolchain (no CMake / Ninja / system compiler).

Two halves:
  - build_runtime(): our runtime (runtime/*.c, with its shaders and images
    baked in) as a static library. Done once per release by the developer
    (it needs glslc for the shaders); players get the library prebuilt.
  - build_game(): what a player's machine does: compile the C recompiled from
    their own disc and link it with the runtime library. Reports progress.

Platforms: "linux" (x86_64, glibc 2.31 and newer) and "windows" (x86_64, MinGW ABI).
The developer builds both from Linux; a player's machine builds for itself (HOST).
"""
import concurrent.futures as cf
import os
import re
import shutil
import subprocess
import sys

# the source checkout, or the packaged app's folder (PyInstaller: next to the executable)
ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

CFLAGS = ["-O2", "-std=gnu11", "-fno-strict-aliasing", "-fwrapv"]
# the oldest Linux the game runs on: glibc 2.31 (Ubuntu 20.04, Debian 11 and newer).
# Everything (runtime, SDL3, the game, the CHD helper) is built against it.
TARGETS = {"linux": "x86_64-linux-gnu.2.31", "windows": "x86_64-windows-gnu"}
HOST = "windows" if os.name == "nt" else "linux"
SDL_VERSION = "3.4.18"


def target(platform=HOST):
    return ["-target", TARGETS[platform]]


def exe_suffix(platform=HOST):
    return ".exe" if platform == "windows" else ""


class BuildError(Exception):
    pass


def find_zig():
    """The bundled zig (build/toolchain/zig-*/zig, or next to the app), else one on PATH."""
    exe = "zig.exe" if os.name == "nt" else "zig"
    for base in (os.path.join(ROOT, "toolchain"), os.path.join(ROOT, "build", "toolchain")):
        if os.path.isdir(base):
            for d in sorted(os.listdir(base), reverse=True):
                p = os.path.join(base, d, exe)
                if d.startswith("zig") and os.path.exists(p):
                    return p
    for d in os.environ.get("PATH", "").split(os.pathsep):
        p = os.path.join(d, exe)
        if os.path.exists(p):
            return p
    return None


def _jobs():
    return max(1, os.cpu_count() or 1)


def _run(cmd, **kw):
    # no console window flashing up for every compile on Windows
    if os.name == "nt":
        kw.setdefault("creationflags", 0x08000000)  # CREATE_NO_WINDOW
    return subprocess.run(cmd, **kw)


def _current(job):
    src, obj = job[:2]
    cached = len(job) > 3 and job[3]  # obj is named by a fingerprint of src + flags: existing means current
    return os.path.exists(obj) and (cached or os.path.getmtime(obj) >= os.path.getmtime(src))


def _compile_all(zig, jobs, progress, label, platform=HOST, log=None):
    """jobs: (source, object, flags[, cached]); progress(done, total, label, fraction)
    counts only the sources that need compiling, the fraction by their size (a big
    file takes longer). Returns how many did."""
    # the biggest first: the largest (dispatch.c, an eighth of the game code) would
    # otherwise start last and keep the build waiting on it alone for a minute
    todo = sorted((j for j in jobs if not _current(j)), key=lambda j: -os.path.getsize(j[0]))
    done, total, errors = 0, len(todo), []
    size_done, size_total = 0, sum(os.path.getsize(j[0]) for j in todo) or 1

    def one(job):
        src, obj, flags = job[:3]
        r = _run([zig, "cc", "-c", *target(platform), *flags, src, "-o", obj], capture_output=True, text=True)
        return src, r.returncode, r.stderr

    if log and len(todo) < len(jobs):
        log(f"{len(jobs) - len(todo)} of {len(jobs)} files unchanged since the last build: reused")
    if log and todo:
        log(f"compiling {len(todo)} files, {_jobs()} at a time")
    with cf.ThreadPoolExecutor(_jobs()) as ex:
        for fut in cf.as_completed([ex.submit(one, j) for j in todo]):  # as each one finishes
            src, rc, err = fut.result()
            done += 1
            size_done += os.path.getsize(src)
            if log:
                log(f"{'FAILED' if rc else 'compiled'} {os.path.basename(src)} ({done} of {total})")
            if rc:
                errors.append(f"{os.path.basename(src)}:\n{err[-2000:]}")
            if progress:
                progress(done, total, label, size_done / size_total)
    if errors:
        raise BuildError("compiling failed:\n" + "\n".join(errors[:3]))
    return total


# ---------------- developer side: the runtime library and its dependencies ----------------

def runtime_sources():
    cm = open(os.path.join(ROOT, "CMakeLists.txt")).read()
    block = cm[cm.index("set(RUNTIME_SRC"):cm.index("if(RENDERER")]
    return re.findall(r"runtime/\w+\.c", block) + ["runtime/vk_renderer.c", "runtime/pvr_tex.c"]


def build_runtime(out_lib, gen_inc_dirs, sdl_include, vulkan_include, zig=None, extra_cflags=(), progress=None,
                  platform=HOST):
    """Compile runtime/*.c into a static library. gen_inc_dirs: where the shader
    and image .inc files are (the CMake build makes them: build/cmake/shaders, build/cmake/assets).
    sdl_include / vulkan_include: SDL3's and Vulkan's headers (the same for every platform)."""
    zig = zig or find_zig()
    if not zig:
        raise BuildError("zig not found")
    obj_dir = out_lib + ".obj"
    os.makedirs(obj_dir, exist_ok=True)
    inc = (["-I" + os.path.join(ROOT, "runtime")] + ["-I" + d for d in gen_inc_dirs]
           + ["-I" + sdl_include, "-I" + vulkan_include])
    jobs = [(os.path.join(ROOT, s), os.path.join(obj_dir, os.path.basename(s)[:-2] + ".o"),
             CFLAGS + list(extra_cflags) + inc) for s in runtime_sources()]
    _compile_all(zig, jobs, progress, "runtime", platform)
    if os.path.exists(out_lib):
        os.remove(out_lib)
    subprocess.check_call([zig, "ar", "rcs", out_lib] + [j[1] for j in jobs])
    return out_lib


def build_sdl3(src_dir, work_dir, out_dir, zig=None, platform=HOST):
    """SDL3 as a shared library (CMake drives zig through small wrapper scripts).
    linux: libSDL3.so.0 against TARGETS' glibc; its X11 / Wayland / audio backends are
    loaded at run time, so the system's headers are only needed here.
    windows: SDL3.dll and its import library libSDL3.dll.a."""
    zig = zig or find_zig()
    cc_dir = os.path.join(work_dir, "cc")
    os.makedirs(cc_dir, exist_ok=True)
    extra = " -idirafter /usr/include" if platform == "linux" else ""
    tools = {}
    for name, cmd in (("zcc", f"cc -target {TARGETS[platform]}{extra}"),
                      ("zcxx", f"c++ -target {TARGETS[platform]}{extra}"), ("zar", "ar"), ("zranlib", "ranlib"),
                      ("zrc", "rc")):
        tools[name] = os.path.join(cc_dir, name)
        with open(tools[name], "w") as f:
            f.write(f'#!/bin/sh\nexec "{zig}" {cmd} "$@"\n')
        os.chmod(tools[name], 0o755)
    obj = os.path.join(work_dir, "obj")
    args = ["cmake", "-S", src_dir, "-B", obj, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_C_COMPILER=" + tools["zcc"], "-DCMAKE_CXX_COMPILER=" + tools["zcxx"], "-DCMAKE_AR=" + tools["zar"],
            "-DCMAKE_RANLIB=" + tools["zranlib"], "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF",
            "-DSDL_TESTS=OFF", "-DSDL_EXAMPLES=OFF", "-DSDL_TEST_LIBRARY=OFF"]
    if platform == "windows":
        args += ["-DCMAKE_SYSTEM_NAME=Windows", "-DCMAKE_SYSTEM_PROCESSOR=x86_64",
                 "-DCMAKE_RC_COMPILER=" + tools["zrc"]]
    subprocess.check_call(args, stdout=subprocess.DEVNULL)
    subprocess.check_call(["ninja", "-C", obj], stdout=subprocess.DEVNULL)
    os.makedirs(out_dir, exist_ok=True)
    if platform == "windows":
        subprocess.check_call(["strip", "-o", os.path.join(out_dir, "SDL3.dll"), os.path.join(obj, "SDL3.dll")])
        shutil.copy2(os.path.join(obj, "libSDL3.dll.a"), out_dir)
    else:
        subprocess.check_call(["strip", "-o", os.path.join(out_dir, "libSDL3.so.0"),
                               os.path.join(obj, "libSDL3.so.0")])
    return out_dir


def vulkan_stub(runtime_lib, out_dir, zig=None, platform=HOST):
    """What the game links against for Vulkan, so that players need no Vulkan SDK:
    linux: a stand-in libvulkan.so.1 (the same function names, no code); the game then
    uses the system's real loader, installed with the graphics driver. Never ship it
    next to the game.
    windows: an import library for vulkan-1.dll (the driver's loader, in System32)."""
    zig = zig or find_zig()
    syms = subprocess.run(["nm", "-u", runtime_lib], capture_output=True, text=True, check=True).stdout.split()
    names = sorted({s for s in syms if re.fullmatch(r"vk[A-Z]\w+", s)})
    os.makedirs(out_dir, exist_ok=True)
    if platform == "windows":
        out = os.path.join(out_dir, "libvulkan-1.a")
        deff = out + ".def"
        with open(deff, "w") as f:
            f.write("LIBRARY vulkan-1.dll\nEXPORTS\n" + "".join(f"    {n}\n" for n in names))
        subprocess.check_call([zig, "dlltool", "-d", deff, "-l", out, "-m", "i386:x86-64"])
        os.remove(deff)
        return out
    out = os.path.join(out_dir, "libvulkan.so.1")
    src = out + ".c"
    with open(src, "w") as f:
        f.write("/* generated by tools/zig_build.py: link-time stand-in for the Vulkan loader */\n")
        f.writelines(f"void {n}(void) {{}}\n" for n in names)
    subprocess.check_call([zig, "cc", *target(platform), "-shared", "-Wl,-soname,libvulkan.so.1", src, "-o", out])
    os.remove(src)
    return out


def windows_resources(out_res, icon_png, zig=None):
    """The game's Windows resources, compiled to a .res it links in: the manifest
    (runtime/win/sfa3.manifest: UTF-8 file names, long paths) and the icon, made from
    icon_png (assets/icon.png) as a multi-size .ico next to out_res (needs ImageMagick)."""
    zig = zig or find_zig()
    out_dir = os.path.dirname(out_res)
    ico = os.path.join(out_dir, "sfa3.ico")
    subprocess.check_call(["magick", icon_png, "-define", "icon:auto-resize=256,64,48,32,16", ico])
    shutil.copy2(os.path.join(ROOT, "runtime", "win", "sfa3.manifest"), out_dir)
    rc = os.path.join(out_dir, "sfa3.rc")
    with open(rc, "w") as f:
        f.write('1 ICON "sfa3.ico"\n1 24 "sfa3.manifest"\n')
    subprocess.check_call([zig, "rc", "/fo", out_res, rc], cwd=out_dir)
    for tmp in (rc, os.path.join(out_dir, "sfa3.manifest")):
        os.remove(tmp)
    return out_res


# ---------------- player side: the game ----------------

def game_link_args(lib_dir, platform=HOST):
    """Link arguments for the game, given the folder with the prebuilt libraries:
    SDL3 (installed next to the game), Vulkan (the driver's loader) and system libraries."""
    if platform == "windows":
        return [os.path.join(lib_dir, "sfa3.res"), os.path.join(lib_dir, "libSDL3.dll.a"),
                os.path.join(lib_dir, "link", "libvulkan-1.a"),
                "-lws2_32", "-liphlpapi", "-Wl,--subsystem,windows", "-Wl,--stack,8388608"]
    return [os.path.join(lib_dir, "libSDL3.so.0"), os.path.join(lib_dir, "link", "libvulkan.so.1"), "-lm",
            "-Wl,-rpath,$ORIGIN/lib", "-rdynamic"]


def build_game(gen_dir, runtime_lib, out_exe, libs, zig=None, progress=None, log=None, strip=True, extra_cflags=(),
               platform=HOST, cache_dir=None):
    """Compile the recompiled C in gen_dir (its sources.txt) and link it with the
    runtime library. libs: game_link_args(). progress(fraction, message): the whole
    build as one fraction.
    cache_dir: keep the compiled objects there, each named by a fingerprint of its
    source, the flags and the compiler, so an update only compiles what changed
    (usually nothing: most updates only bring a new runtime library) and relinks."""
    import hashlib
    zig = zig or find_zig()
    if not zig:
        raise BuildError("the bundled compiler (zig) is missing")
    if not os.path.exists(runtime_lib):
        raise BuildError(f"the runtime library is missing: {runtime_lib}")
    names = open(os.path.join(gen_dir, "sources.txt")).read().split()
    obj_dir = cache_dir or os.path.join(os.path.dirname(out_exe), "obj")
    os.makedirs(obj_dir, exist_ok=True)
    inc = ["-w", "-I" + gen_dir, "-I" + os.path.join(ROOT, "runtime")]
    flags = CFLAGS + list(extra_cflags) + inc
    jobs = []
    for n in names:
        src = os.path.join(gen_dir, n)
        if cache_dir:
            h = hashlib.sha1()
            for part in (_zig_version(zig), TARGETS[platform], " ".join(CFLAGS + list(extra_cflags))):
                h.update(part.encode() + b"\0")
            with open(src, "rb") as f:
                h.update(f.read())
            h.update(_header_fingerprint(gen_dir))
            jobs.append((src, os.path.join(obj_dir, f"{n[:-2]}-{h.hexdigest()[:16]}.o"), flags, True))
        else:
            jobs.append((src, os.path.join(obj_dir, n[:-2] + ".o"), flags))
    # one bar for the whole build: compiling, then linking, weighted by how long
    # each took last time (linking is a single step: it moves on that estimate)
    import json
    import time
    timings_path = os.path.join(obj_dir, "timings.json")
    try:
        timings = json.load(open(timings_path))
    except (OSError, ValueError):
        timings = {}
    per_file, link_est = float(timings.get("per_file", 1.0)), float(timings.get("link", 15.0))
    todo = sum(1 for j in jobs if not _current(j))
    compile_est = todo * per_file
    share = compile_est / (compile_est + link_est) if compile_est else 0.0

    def report(frac, msg):
        if progress:
            progress(min(max(frac, 0.0), 1.0), msg)

    t0 = time.monotonic()
    compiled = _compile_all(zig, jobs, lambda d, t, _l, f: report(share * f, f"Compiling ({d} of {t})"),
                            "game", platform, log=log)
    if compiled:
        timings["per_file"] = (time.monotonic() - t0) / compiled
    if log:
        log(f"game code: {compiled} of {len(jobs)} files compiled, {len(jobs) - compiled} reused")
        log(f"linking {os.path.basename(out_exe)} (the game code and the port's runtime)...")
    report(share, "Linking")
    cmd = [zig, "cc", *target(platform), "-o", out_exe, *[j[1] for j in jobs], runtime_lib, *libs]
    if strip:
        cmd.append("-s")
    kw = {"creationflags": 0x08000000} if os.name == "nt" else {}
    t1 = time.monotonic()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kw)
    while True:
        try:
            _, err = p.communicate(timeout=0.25)
            break
        except subprocess.TimeoutExpired:  # creep towards the end, never quite reaching it
            report(share + (1 - share) * min(0.97, (time.monotonic() - t1) / link_est), "Linking")
    if p.returncode:
        raise BuildError("linking failed:\n" + err[-3000:])
    timings["link"] = time.monotonic() - t1
    if cache_dir:  # drop objects this build no longer uses
        used = {os.path.basename(j[1]) for j in jobs}
        for f in os.listdir(cache_dir):
            if f.endswith(".o") and f not in used:
                os.remove(os.path.join(cache_dir, f))
    try:
        json.dump(timings, open(timings_path, "w"))
    except OSError:
        pass
    if log:
        log(f"linked: {out_exe} ({os.path.getsize(out_exe) // (1 << 20)} MB)")
    report(1.0, "Linking")
    return out_exe


_ZIG_VERSIONS = {}


def _zig_version(zig):
    """The compiler's version, not where it is: moving the app keeps the cache."""
    if zig not in _ZIG_VERSIONS:
        r = _run([zig, "version"], capture_output=True, text=True)
        _ZIG_VERSIONS[zig] = "zig " + (r.stdout.strip() if r.returncode == 0 else zig)
    return _ZIG_VERSIONS[zig]


_HEADERS = {}


def _header_fingerprint(gen_dir):
    """The runtime headers the generated C includes (directly or through each
    other): a change to one of them recompiles everything. Other runtime headers
    don't matter here, so changing them never touches the cached objects."""
    if gen_dir not in _HEADERS:
        import hashlib
        import re
        rt = os.path.join(ROOT, "runtime")
        inc = re.compile(rb'^\s*#\s*include\s+"([^"]+)"', re.M)
        todo = set()
        for name in os.listdir(gen_dir):
            if name.endswith((".c", ".h")):
                with open(os.path.join(gen_dir, name), "rb") as f:
                    todo.update(m.decode() for m in inc.findall(f.read()))
        seen = {}
        while todo:
            name = todo.pop()
            path = os.path.join(rt, name)
            if name in seen or not os.path.isfile(path):
                continue  # a generated header (sources are hashed themselves)
            with open(path, "rb") as f:
                seen[name] = f.read()
            todo.update(m.decode() for m in inc.findall(seen[name]))
        h = hashlib.sha1()
        for name in sorted(seen):
            h.update(name.encode() + b"\0" + seen[name])
        _HEADERS[gen_dir] = h.digest()
    return _HEADERS[gen_dir]
