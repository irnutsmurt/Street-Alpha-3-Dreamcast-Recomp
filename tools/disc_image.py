#!/usr/bin/env python3
"""The player's disc image, in any supported format, as a GDI.

The port reads its disc through a GDI (track list + track files): the setup
pipeline extracts files from it and the game reads sectors from it while it
runs (build/disc_path.txt). Other formats are turned into one:

  .gdi  used as is
  .cue  (Redump CUE/BIN) a GDI pointing at the BIN files themselves, with each
        track's pregap skipped through the GDI's byte-offset column; nothing
        is copied
  .chd  unpacked into GDI track files by the chd2gdi helper (libchdr)
"""
import os
import re
import shutil
import subprocess

SECTOR = 2352
HD_AREA_LBA = 45000  # the GD-ROM's high-density area starts here


class ImageError(Exception):
    pass


def _msf(s):
    m, sec, f = (int(x) for x in s.split(":"))
    return (m * 60 + sec) * 75 + f


def cue_to_gdi(cue, out_gdi):
    """Write a GDI equivalent to a Redump Dreamcast CUE/BIN set."""
    base = os.path.dirname(os.path.abspath(cue))
    tracks, cur_file, hd = [], None, False
    for raw in open(cue, encoding="utf-8", errors="replace"):
        line = raw.strip()
        if line.upper().startswith("REM HIGH-DENSITY AREA"):
            hd = True
        m = re.match(r'FILE\s+"(.+)"\s+\w+', line, re.I) or re.match(r"FILE\s+(\S+)\s+\w+", line, re.I)
        if m:
            cur_file = os.path.join(base, m.group(1))
            continue
        m = re.match(r"TRACK\s+(\d+)\s+(\S+)", line, re.I)
        if m:
            if cur_file is None:
                raise ImageError("CUE: a TRACK before any FILE")
            mode = m.group(2).upper()
            if mode not in ("AUDIO", "MODE1/2352", "MODE2/2352"):
                raise ImageError(f"CUE: unsupported track mode {mode} (a Redump dump uses 2352-byte sectors)")
            tracks.append({"file": cur_file, "audio": mode == "AUDIO", "hd": hd, "index": {}})
            continue
        m = re.match(r"INDEX\s+(\d+)\s+(\d+:\d+:\d+)", line, re.I)
        if m and tracks:
            tracks[-1]["index"][int(m.group(1))] = _msf(m.group(2))
    if not tracks:
        raise ImageError("CUE: no tracks")
    lines, lba, prev_end = [], 0, None
    for n, t in enumerate(tracks, 1):
        if not os.path.exists(t["file"]):
            raise ImageError(f"missing track file: {t['file']}")
        start = t["index"].get(1, 0)  # the track's first sector within its file (after any pregap)
        if t["hd"] and (n == 1 or not tracks[n - 2]["hd"]):
            lba = HD_AREA_LBA
        elif prev_end is not None:
            lba = prev_end + (start - t["index"].get(0, start))  # the pregap before it
        sectors = os.path.getsize(t["file"]) // SECTOR - start
        lines.append(f'{n} {lba} {0 if t["audio"] else 4} {SECTOR} "{t["file"]}" {start * SECTOR}')
        prev_end = lba + sectors
    with open(out_gdi, "w") as f:
        f.write(f"{len(lines)}\n" + "\n".join(lines) + "\n")
    return out_gdi


def chd_to_gdi(chd, out_dir, helper, progress=None):
    """Unpack a CHD into GDI track files with the chd2gdi helper (libchdr)."""
    if not helper or not os.path.exists(helper):
        raise ImageError("the CHD helper (chd2gdi) is missing")
    os.makedirs(out_dir, exist_ok=True)
    p = subprocess.Popen([helper, chd, out_dir], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         creationflags=0x08000000 if os.name == "nt" else 0)  # CREATE_NO_WINDOW
    last = ""
    for line in p.stdout:
        last = line.strip()
        m = re.match(r"progress (\d+)/(\d+)", last)
        if m and progress:
            progress(int(m.group(1)), int(m.group(2)))
    if p.wait() != 0:
        raise ImageError(f"could not unpack the CHD: {last}")
    return os.path.join(out_dir, "disc.gdi")


def to_gdi(image, work_dir, chd_helper=None, progress=None):
    """The GDI to use for `image`; written under work_dir when it needs converting."""
    ext = os.path.splitext(image)[1].lower()
    if not os.path.exists(image):
        raise ImageError(f"not found: {image}")
    if ext == ".gdi":
        return os.path.abspath(image)
    os.makedirs(work_dir, exist_ok=True)
    if ext == ".cue":
        return cue_to_gdi(image, os.path.join(work_dir, "disc.gdi"))
    if ext == ".chd":
        out = os.path.join(work_dir, "chd")
        shutil.rmtree(out, ignore_errors=True)
        return chd_to_gdi(image, out, chd_helper, progress)
    raise ImageError(f"unsupported disc image '{ext}' (use .gdi, .cue or .chd)")
