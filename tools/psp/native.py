"""The add-on's slow conversions in C (tools/sfa3conv.c), through ctypes.

The library is one of the prebuilt parts (sfa3port.py dev-libs: build/tools/<platform>/,
in the app's bin/). Without it, or with SFA3_NO_NATIVE set, LIB is None and pac.py /
sound.py run their Python versions, which give the same bytes. VERSION goes into the
add-on's cache keys (cache.py): a change to the C's output is a new version.
"""
import ctypes
import os
import sys

NAME = "sfa3conv.dll" if os.name == "nt" else "libsfa3conv.so"
HOST = "windows" if os.name == "nt" else "linux"
# the packaged app's folder (PyInstaller: next to the executable), or the source checkout
ROOT = (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

u8p, i32p, f64p = ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_double)
size_t = ctypes.c_size_t


def _load():
    if os.environ.get("SFA3_NO_NATIVE"):
        return None
    for path in (os.path.join(ROOT, "bin", NAME), os.path.join(ROOT, "build", "tools", HOST, NAME)):
        if os.path.exists(path):
            try:
                lib = ctypes.CDLL(path)
            except OSError:
                continue
            lib.sfa3conv_version.restype = ctypes.c_int
            lib.sfa3conv_lz_opt.argtypes = [u8p, size_t, u8p]
            lib.sfa3conv_lz_opt.restype = size_t
            lib.sfa3conv_tiles_room.argtypes = [u8p, size_t]
            lib.sfa3conv_tiles_room.restype = size_t
            lib.sfa3conv_convert_tiles.argtypes = [u8p, size_t, u8p, size_t]
            lib.sfa3conv_convert_tiles.restype = size_t
            lib.sfa3conv_vag_decode.argtypes = [u8p, size_t, i32p]
            lib.sfa3conv_vag_decode.restype = size_t
            lib.sfa3conv_resample.argtypes = [i32p, size_t, ctypes.c_double, f64p, ctypes.c_int, ctypes.c_int, i32p, size_t]
            lib.sfa3conv_resample.restype = None
            lib.sfa3conv_aica_encode.argtypes = [i32p, size_t, u8p]
            lib.sfa3conv_aica_encode.restype = None
            for f in (lib.sfa3conv_twiddle, lib.sfa3conv_untwiddle):
                f.argtypes = [u8p, u8p]
                f.restype = None
            lib.sfa3conv_scale.argtypes = [i32p, size_t, ctypes.c_double, i32p]
            lib.sfa3conv_scale.restype = None
            return lib
    return None


LIB = _load()
VERSION = LIB.sfa3conv_version() if LIB else 0


def _bytes_in(data):
    return (ctypes.c_uint8 * len(data)).from_buffer_copy(data)


def lz_opt(data):
    out = (ctypes.c_uint8 * (2 * len(data) + len(data) // 8 + 2))()
    n = LIB.sfa3conv_lz_opt(_bytes_in(data), len(data), out)
    if not n and data:
        raise ValueError("lz_opt: more than 4096 bytes")
    return bytes(out[:n])


def convert_tiles(sec):
    src = _bytes_in(sec)
    room = LIB.sfa3conv_tiles_room(src, len(sec))
    out = (ctypes.c_uint8 * max(room, 1))()
    n = LIB.sfa3conv_convert_tiles(src, len(sec), out, room)
    if not n:
        raise ValueError("a damaged sprite tile section")
    return bytes(out[:n])


def vag_decode(data):
    out = (ctypes.c_int32 * (len(data) // 16 * 28 + 1))()
    n = LIB.sfa3conv_vag_decode(_bytes_in(data), len(data), out)
    return list(out[:n])


def resample(x, ratio, table, phases, half, count):
    src = (ctypes.c_int32 * max(len(x), 1))(*x)
    flat = (ctypes.c_double * (phases * 2 * half))(*(v for row in table for v in row))
    out = (ctypes.c_int32 * max(count, 1))()
    LIB.sfa3conv_resample(src, len(x), ratio, flat, phases, half, out, count)
    return list(out[:count])


def aica_encode(samples):
    src = (ctypes.c_int32 * max(len(samples), 1))(*samples)
    out = (ctypes.c_uint8 * ((len(samples) + 1) // 2))()
    LIB.sfa3conv_aica_encode(src, len(samples), out)
    return bytes(out)


def twiddle(tile):
    out = (ctypes.c_uint8 * 128)()
    LIB.sfa3conv_twiddle(_bytes_in(tile[:128].ljust(128, b"\0")), out)
    return bytes(out)


def untwiddle(tile):
    out = (ctypes.c_uint8 * 128)()
    LIB.sfa3conv_untwiddle(_bytes_in(tile[:128].ljust(128, b"\0")), out)
    return bytes(out)


def scale(samples, gain):
    src = (ctypes.c_int32 * max(len(samples), 1))(*samples)
    out = (ctypes.c_int32 * max(len(samples), 1))()
    LIB.sfa3conv_scale(src, len(samples), gain, out)
    return list(out[:len(samples)])
