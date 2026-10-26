"""Fingerprints of the setup's own code, so that an update can tell what it has to do again.

Each tool module (tools/, tools/recomp/, tools/psp/) has a fingerprint: its source and
the data files it reads (DATA). closure(*modules) is the fingerprint of the modules and
everything of ours they import, directly or not (imports inside functions too): a step
whose closure, inputs and settings are the same as last time made the same output.

A source checkout computes them from the files. The packaged app has no module sources
(PyInstaller keeps bytecode): make_bundle.py writes them to stamps.json, which the app
carries; an app without it never skips anything (closure() returns None).

    python3 tools/stamps.py [out.json]     (prints them, or writes the app's file)
"""
import ast
import hashlib
import json
import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
DIRS = [TOOLS, os.path.join(TOOLS, "recomp"), os.path.join(TOOLS, "psp")]
# files a module reads besides its imports
DATA = {
    "recompile": [os.path.join("recomp", "extra_entries.txt")],
    "build_char": [os.path.join("psp", "manual_maps.json")],
    "native": ["sfa3conv.c"],  # the C library's source: what it computes
}
FILE = "stamps.json"


def scan():
    """{module: {"hash": sha1 of its source and data files, "imports": [our modules it imports]}}"""
    sources = {}
    for d in DIRS:
        for f in sorted(os.listdir(d)):
            if f.endswith(".py"):
                sources.setdefault(f[:-3], os.path.join(d, f))
    out = {}
    for name, path in sources.items():
        src = open(path, "rb").read()
        h = hashlib.sha1(src)
        for rel in DATA.get(name, ()):
            h.update(b"\0" + rel.encode() + b"\0" + open(os.path.join(TOOLS, rel), "rb").read())
        imports = set()
        for node in ast.walk(ast.parse(src, path)):
            if isinstance(node, ast.Import):
                imports.update(a.name.split(".")[0] for a in node.names)
            elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
                imports.add(node.module.split(".")[0])
        out[name] = {"hash": h.hexdigest(), "imports": sorted(i for i in imports if i in sources and i != name)}
    return out


_MODULES = None


def modules():
    """the fingerprints: the app's stamps.json, or the checkout's files (None: unknown)"""
    global _MODULES
    if _MODULES is None:
        if getattr(sys, "frozen", False):
            path = os.path.join(getattr(sys, "_MEIPASS", os.path.dirname(sys.executable)), FILE)
            try:
                _MODULES = json.load(open(path))
            except (OSError, ValueError):
                _MODULES = {}
        else:
            _MODULES = scan()
    return _MODULES or None


def closure(*names):
    """the fingerprint of these modules and all of ours they import, or None (unknown)"""
    mods = modules()
    if not mods or any(n not in mods for n in names):
        return None
    seen, todo = set(), list(names)
    while todo:
        n = todo.pop()
        if n not in seen:
            seen.add(n)
            todo.extend(mods[n]["imports"])
    h = hashlib.sha1()
    for n in sorted(seen):
        h.update(f"{n}={mods[n]['hash']}\n".encode())
    return h.hexdigest()


def key(*parts):
    """a fingerprint of several values (None in any: None, never matching)"""
    if any(p is None for p in parts):
        return None
    h = hashlib.sha1()
    for p in parts:
        h.update((p if isinstance(p, bytes) else str(p).encode()) + b"\0")
    return h.hexdigest()


if __name__ == "__main__":
    m = scan()
    if len(sys.argv) > 1:
        json.dump(m, open(sys.argv[1], "w"), indent=1, sort_keys=True)
    else:
        for n in sorted(m):
            print(f"{n:16} {m[n]['hash'][:12]}  {' '.join(m[n]['imports'])}")
