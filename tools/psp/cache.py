"""The add-on's work kept between runs: an update that didn't change a converter reuses
what it made last time instead of converting again.

The setup (pipeline.py) turns it on with configure(<data>/addon_cache, inputs): inputs
is a fingerprint of what the converters read (the two discs). An entry is keyed by its
name, the fingerprint of the code that makes it (stamps.closure of its modules, the C
library's version), the inputs and the call's own key parts; a change to any is a new
key. Without configure() (developers running build_char.py) nothing is cached.

    value(name, modules, parts, make)        make() -> anything picklable
    files(name, modules, parts, out_dir, make) make(dir) writes files into dir; they're
                                             kept and written to out_dir again next time
finish() removes the entries this run didn't use.
"""
import hashlib
import os
import pickle
import shutil
import sys
import tempfile

try:
    import stamps
except ImportError:  # build_char.py run by itself: tools/ isn't on the path
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    import stamps
import native

DIR = None
INPUTS = None
_used = set()


def configure(directory, inputs):
    """cache in directory (None: off); inputs: the fingerprint of the discs (None: off)"""
    global DIR, INPUTS
    DIR, INPUTS = (directory, inputs) if directory and inputs else (None, None)
    _used.clear()
    if DIR:
        os.makedirs(DIR, exist_ok=True)


def inputs(*paths):
    """the fingerprint of the discs from their main programs (1ST_READ.BIN, BOOT.BIN): each is
    one release, which fixes its other files"""
    h = hashlib.sha1()
    for p in paths:
        with open(p, "rb") as f:
            h.update(hashlib.sha1(f.read()).digest())
    return h.hexdigest()


def _part(p):
    if isinstance(p, (bytes, bytearray, memoryview)):
        return hashlib.sha1(p).hexdigest()
    return repr(p)


def _path(name, modules, parts):
    if not DIR:
        return None
    k = stamps.key(name, stamps.closure(*modules), native.VERSION, INPUTS, *(_part(p) for p in parts))
    return k and os.path.join(DIR, f"{name}-{k[:24]}.pkl")


def _load(path):
    try:
        with open(path, "rb") as f:
            v = pickle.load(f)
        _used.add(os.path.basename(path))
        return True, v
    except (OSError, EOFError, pickle.UnpicklingError, AttributeError, ValueError):
        return False, None


def _store(path, v):
    tmp = path + ".tmp"
    try:
        with open(tmp, "wb") as f:
            pickle.dump(v, f, protocol=pickle.HIGHEST_PROTOCOL)
        os.replace(tmp, path)
        _used.add(os.path.basename(path))
    except OSError:
        pass  # a cache: the work is done either way


def value(name, modules, parts, make):
    """make()'s value, from the cache when its key is there"""
    path = _path(name, modules, parts)
    if path:
        hit, v = _load(path)
        if hit:
            return v
    v = make()
    if path:
        _store(path, v)
    return v


def files(name, modules, parts, out_dir, make):
    """make(dir) writes files into dir and returns a value: both kept; on a hit the files are
    written to out_dir from the cache and the value returned"""
    path = _path(name, modules, parts)
    if path:
        hit, v = _load(path)
        if hit:
            written, ret = v
            for n, data in written.items():
                with open(os.path.join(out_dir, n), "wb") as f:
                    f.write(data)
            return ret
    if not path:
        return make(out_dir)
    tmp = tempfile.mkdtemp(prefix=name + "-", dir=DIR)
    try:
        ret = make(tmp)
        written = {}
        for n in sorted(os.listdir(tmp)):
            with open(os.path.join(tmp, n), "rb") as f:
                written[n] = f.read()
            shutil.copy2(os.path.join(tmp, n), os.path.join(out_dir, n))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    _store(path, (written, ret))
    return ret


def finish():
    """drop the entries this run didn't use (an older version's work)"""
    if not DIR:
        return
    for f in os.listdir(DIR):
        p = os.path.join(DIR, f)
        if f not in _used:
            shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else os.remove(p)
