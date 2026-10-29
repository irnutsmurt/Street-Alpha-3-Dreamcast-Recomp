#!/usr/bin/env python3
"""The setup app updating itself from the project's GitHub releases.

    latest()                   -> (version, download URL) of the newest release, or None
    download_and_unpack(...)   -> the new app's executable, unpacked next to this one
    remove_old(folder)         -> the previous version's folder, once it has exited

A release is tagged v<VERSION> (tools/version.py) and carries
SFA3-Setup-linux-x86_64.tar.xz and SFA3-Setup-windows-x86_64.zip.

The update is applied in place, in this app's own folder:
    stage(url, version)        downloads and unpacks the new version into <app>/.update/
    apply_and_restart(...)     puts it in place and starts it (the caller then exits):
        Linux: replaced right away (a running program's files can be replaced)
        Windows: a small hidden helper script waits for this app to exit, moves the
        old parts aside (each whole, or the update is undone and the old version
        starts again with --update-failed), moves the new ones in and starts them
Only the app's own parts are replaced (APP_PARTS), never anything else in the folder.
"""
import json
import os
import re
import shutil
import subprocess
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


APP_PARTS = ["_internal", "toolchain", "lib", "bin", "runtime", "licenses", APP, "README.txt", "LICENSE.txt"]


def stage(url, version, app_dir, progress=None):
    """Download the release and unpack it into app_dir/.update/<version>/. Returns that folder."""
    root = os.path.join(app_dir, ".update")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)
    archive = os.path.join(root, ASSET)
    with _get(url, 30) as r, open(archive, "wb") as f:
        total, done = int(r.headers.get("Content-Length") or 0), 0
        while chunk := r.read(1 << 20):
            f.write(chunk)
            done += len(chunk)
            if progress:
                progress(done, total)
    unpacked = os.path.join(root, "unpacked")
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(unpacked)
    else:
        with tarfile.open(archive) as t:
            t.extractall(unpacked, filter="data")
    os.remove(archive)
    top = [d for d in os.listdir(unpacked) if os.path.isdir(os.path.join(unpacked, d))]
    if len(top) != 1 or not looks_like_app(os.path.join(unpacked, top[0])):
        raise OSError("the download doesn't contain the setup app")
    new = os.path.join(root, version)
    os.replace(os.path.join(unpacked, top[0]), new)
    shutil.rmtree(unpacked)
    return new


_HELPER = r"""@echo off
rem SFA3 setup: put the downloaded version in place once the old one has exited
:wait
tasklist /FI "PID eq %SFA3_PID%" /NH | find "%SFA3_PID%" >nul
if not errorlevel 1 (
    ping -n 2 127.0.0.1 >nul
    goto wait
)
rem each old part aside first, in one move (a rename: all of it or none), retried while anything still
rem has a file in it open; if one won't go, the ones moved go back and the old version starts again
rem (deleting in place could leave half a folder, and moving the new one onto that put it inside)
mkdir "%SFA3_OLD%" 2>nul
set SFA3_FAILED=
for %%P in (%SFA3_PARTS%) do if not defined SFA3_FAILED call :aside "%%~P"
if defined SFA3_FAILED goto rollback
for %%P in (%SFA3_PARTS%) do if exist "%SFA3_NEW%\%%~P" move /Y "%SFA3_NEW%\%%~P" "%SFA3_APP%\%%~P" >nul
start "" "%SFA3_APP%\%SFA3_EXE%" --after-update "" "%SFA3_INSTALL%"
rmdir /S /Q "%SFA3_APP%\.update"
goto :eof

:rollback
for %%P in (%SFA3_PARTS%) do if exist "%SFA3_OLD%\%%~P" move /Y "%SFA3_OLD%\%%~P" "%SFA3_APP%\%%~P" >nul
start "" "%SFA3_APP%\%SFA3_EXE%" --update-failed
goto :eof

:aside
if not exist "%SFA3_NEW%\%~1" goto :eof
for /L %%i in (1,1,30) do (
    if exist "%SFA3_APP%\%~1" (
        move "%SFA3_APP%\%~1" "%SFA3_OLD%\%~1" >nul 2>&1
        if exist "%SFA3_APP%\%~1" ping -n 2 127.0.0.1 >nul
    )
)
if exist "%SFA3_APP%\%~1" set SFA3_FAILED=1
goto :eof
"""


def apply_and_restart(new, app_dir, install):
    """Replace this app with the staged version `new` and start it (with
    --after-update, so it updates the game in `install`). The caller must exit
    right after this returns."""
    new, app_dir = os.path.abspath(new), os.path.abspath(app_dir)
    if not looks_like_app(app_dir):
        raise OSError(f"{app_dir} doesn't look like the setup app; not updating it")
    dirs = [p for p in APP_PARTS if os.path.isdir(os.path.join(new, p))]
    files = [p for p in APP_PARTS if os.path.isfile(os.path.join(new, p))]
    if os.name == "nt":
        helper = os.path.join(app_dir, ".update", "apply.bat")
        with open(helper, "w", newline="\r\n") as f:
            f.write(_HELPER)
        env = dict(os.environ, SFA3_PID=str(os.getpid()), SFA3_NEW=new, SFA3_APP=app_dir, SFA3_EXE=APP,
                   SFA3_INSTALL=install, SFA3_PARTS=" ".join(dirs + files),
                   SFA3_OLD=os.path.join(app_dir, ".update", "old"))
        # a hidden console (its commands share it: no windows flash up), in its own
        # process group so that it outlives this process
        subprocess.Popen(["cmd", "/c", helper], env=env, cwd=os.path.dirname(app_dir),
                         creationflags=0x08000000 | 0x00000200)  # CREATE_NO_WINDOW, CREATE_NEW_PROCESS_GROUP
        return
    for p in dirs:  # Linux: running programs keep their open files, so swap now
        dst = os.path.join(app_dir, p)
        shutil.rmtree(dst, ignore_errors=True)
        shutil.move(os.path.join(new, p), dst)
    for p in files:
        os.replace(os.path.join(new, p), os.path.join(app_dir, p))
    os.chmod(os.path.join(app_dir, APP), 0o755)
    shutil.rmtree(os.path.join(app_dir, ".update"), ignore_errors=True)
    subprocess.Popen([os.path.join(app_dir, APP), "--after-update", "", install], cwd=app_dir,
                     start_new_session=True)


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
