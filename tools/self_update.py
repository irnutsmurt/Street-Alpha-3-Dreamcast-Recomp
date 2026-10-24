#!/usr/bin/env python3
"""The setup app updating itself from the project's GitHub releases.

    latest()                   -> (version, download URL) of the newest release, or None
    download_and_unpack(...)   -> the new app's executable, unpacked next to this one
    remove_old(folder)         -> the previous version's folder, once it has exited

A release is tagged v<VERSION> (tools/version.py) and carries
SFA3-Setup-linux-x86_64.tar.xz and SFA3-Setup-windows-x86_64.zip. Each version
unpacks into its own folder (SFA3-Setup-<version>) beside the old one: a running
program can't replace its own files on Windows.
"""
import json
import os
import re
import shutil
import sys
import tarfile
import time
import urllib.request
import zipfile

REPO = "irnutsmurt/Street-Alpha-3-Dreamcast-Recomp"
ASSET = "SFA3-Setup-windows-x86_64.zip" if os.name == "nt" else "SFA3-Setup-linux-x86_64.tar.xz"
APP = "SFA3-Setup.exe" if os.name == "nt" else "SFA3-Setup"


def parse(version):
    """'v0.1.2-test' / '0.1.2' -> (0, 1, 2)"""
    m = re.match(r"v?(\d+)\.(\d+)\.(\d+)", version or "")
    return tuple(int(x) for x in m.groups()) if m else (0, 0, 0)


def _get(url, timeout):
    req = urllib.request.Request(url, headers={"User-Agent": "SFA3-Setup", "Accept": "application/vnd.github+json"})
    return urllib.request.urlopen(req, timeout=timeout)


def latest(timeout=6):
    """The newest release (pre-releases included) that has this platform's download."""
    with _get(f"https://api.github.com/repos/{REPO}/releases?per_page=20", timeout) as r:
        releases = json.load(r)
    best = None
    for rel in releases:
        if rel.get("draft"):
            continue
        url = next((a["browser_download_url"] for a in rel.get("assets", []) if a.get("name") == ASSET), None)
        if url and (best is None or parse(rel["tag_name"]) > parse(best[0])):
            best = (rel["tag_name"].lstrip("v"), url)
    return best


def download_and_unpack(url, version, parent, progress=None):
    """Download the release into parent/ and unpack it as parent/SFA3-Setup-<version>.
    progress(done_bytes, total_bytes). Returns the new app's executable."""
    work = os.path.join(parent, f".sfa3-update-{version}")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    archive = os.path.join(work, ASSET)
    with _get(url, 30) as r, open(archive, "wb") as f:
        total, done = int(r.headers.get("Content-Length") or 0), 0
        while chunk := r.read(1 << 20):
            f.write(chunk)
            done += len(chunk)
            if progress:
                progress(done, total)
    unpacked = os.path.join(work, "unpacked")
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(unpacked)
    else:
        with tarfile.open(archive) as t:
            t.extractall(unpacked, filter="data")
    top = [d for d in os.listdir(unpacked) if os.path.isdir(os.path.join(unpacked, d))]
    if len(top) != 1 or not os.path.exists(os.path.join(unpacked, top[0], APP)):
        raise OSError("the download doesn't contain the setup app")
    dest = os.path.join(parent, f"SFA3-Setup-{version}")
    shutil.rmtree(dest, ignore_errors=True)
    shutil.move(os.path.join(unpacked, top[0]), dest)
    shutil.rmtree(work, ignore_errors=True)
    exe = os.path.join(dest, APP)
    if os.name != "nt":
        os.chmod(exe, 0o755)
    return exe


def looks_like_app(folder):
    """Only ever delete a folder that is a copy of this app."""
    return all(os.path.exists(os.path.join(folder, p)) for p in (APP, "_internal", "toolchain", "lib"))


def remove_old(folder, wait=30):
    """Delete the previous version's folder once that app has exited (its files unlock)."""
    if not folder or not looks_like_app(folder):
        return False
    end = time.time() + wait
    while time.time() < end:
        try:
            shutil.rmtree(folder)
            return True
        except OSError:
            time.sleep(1)
    return False


def current_folder():
    """This app's folder when it runs as the packaged app (None from a source checkout)."""
    return os.path.dirname(sys.executable) if getattr(sys, "frozen", False) else None
