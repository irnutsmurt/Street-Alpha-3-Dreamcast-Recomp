#!/usr/bin/env python3
"""Street Fighter Alpha 3 native PC port: the converter app.

Turns your own disc image (GDI, CUE/BIN or CHD) into the native game, on your
machine: pick the image and an install folder, Convert, Play. Nothing from the
game is shipped with this app; it reads your disc and builds from it locally.
The same steps as `sfa3port.py setup` (tools/pipeline.py).
"""
import os
import queue
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import filedialog, ttk

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools", "recomp"))

import pipeline  # noqa: E402

TITLE = "Street Fighter Alpha 3 — PC port setup"
NEEDED_GB = 4  # unpacked image + extracted disc + build work files, at most


def default_install():
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        return os.path.join(base, "SFA3")
    return os.path.join(os.path.expanduser("~"), "Games", "SFA3")


class App:
    def __init__(self, root):
        self.root = root
        self.events = queue.Queue()
        self.worker = None
        self.game = None
        root.title(TITLE)
        root.minsize(620, 520)
        style = ttk.Style(root)
        if os.name != "nt" and "clam" in style.theme_names():
            style.theme_use("clam")  # Windows keeps its native look
        pad = {"padx": 12, "pady": 6}

        ttk.Label(root, text="Street Fighter Alpha 3", font=("TkDefaultFont", 16, "bold")).pack(anchor="w", padx=12,
                                                                                               pady=(14, 0))
        ttk.Label(root, text="Builds the native PC version from your own Dreamcast disc. Nothing from the game "
                             "is included with this app.", wraplength=590, foreground="#555").pack(anchor="w", **pad)

        form = ttk.Frame(root)
        form.pack(fill="x", **pad)
        form.columnconfigure(1, weight=1)
        ttk.Label(form, text="Disc image").grid(row=0, column=0, sticky="w")
        self.image = tk.StringVar()
        ttk.Entry(form, textvariable=self.image).grid(row=0, column=1, sticky="ew", padx=6)
        ttk.Button(form, text="Browse…", command=self.pick_image).grid(row=0, column=2)
        ttk.Label(form, text="GDI, CUE/BIN (Redump) or CHD of Street Fighter Alpha 3 (USA)", foreground="#777").grid(
            row=1, column=1, sticky="w", padx=6)
        ttk.Label(form, text="Install to").grid(row=2, column=0, sticky="w", pady=(8, 0))
        self.install = tk.StringVar(value=default_install())
        ttk.Entry(form, textvariable=self.install).grid(row=2, column=1, sticky="ew", padx=6, pady=(8, 0))
        ttk.Button(form, text="Browse…", command=self.pick_install).grid(row=2, column=2, pady=(8, 0))

        steps = ttk.LabelFrame(root, text="Progress")
        steps.pack(fill="x", **pad)
        self.marks = {}
        for i, s in enumerate(pipeline.STEPS):
            mark = ttk.Label(steps, text="○", width=2)
            mark.grid(row=i, column=0, padx=(10, 4), pady=1)
            ttk.Label(steps, text=pipeline.STEP_NAMES[s]).grid(row=i, column=1, sticky="w")
            self.marks[s] = mark
        self.bar = ttk.Progressbar(steps, maximum=1.0)
        self.bar.grid(row=len(pipeline.STEPS), column=0, columnspan=2, sticky="ew", padx=10, pady=(8, 2))
        steps.columnconfigure(1, weight=1)
        self.status = ttk.Label(steps, text="Ready.", foreground="#555")
        self.status.grid(row=len(pipeline.STEPS) + 1, column=0, columnspan=2, sticky="w", padx=10, pady=(0, 8))

        self.log_shown = tk.BooleanVar(value=False)
        ttk.Checkbutton(root, text="Show details", variable=self.log_shown, command=self.toggle_log).pack(
            anchor="w", padx=12)
        self.log = tk.Text(root, height=8, wrap="word", state="disabled", font=("TkFixedFont", 9))

        buttons = ttk.Frame(root)
        buttons.pack(side="bottom", fill="x", padx=12, pady=12)
        self.convert_btn = ttk.Button(buttons, text="Convert", command=self.start)
        self.convert_btn.pack(side="right")
        self.play_btn = ttk.Button(buttons, text="Play", command=self.play, state="disabled")
        self.play_btn.pack(side="right", padx=6)
        self.folder_btn = ttk.Button(buttons, text="Open folder", command=self.open_folder, state="disabled")
        self.folder_btn.pack(side="right")
        self.root.after(100, self.poll)
        self.find_existing()

    # ---------------- inputs ----------------

    def pick_image(self):
        p = filedialog.askopenfilename(title="Your disc image", filetypes=[
            ("Disc images", "*.gdi *.cue *.chd *.GDI *.CUE *.CHD"), ("All files", "*")])
        if p:
            self.image.set(p)

    def pick_install(self):
        p = filedialog.askdirectory(title="Install the game to", initialdir=os.path.dirname(self.install.get()))
        if p:
            self.install.set(os.path.join(p, "SFA3") if os.listdir(p) and os.path.basename(p) != "SFA3" else p)

    def find_existing(self):
        exe = os.path.join(self.install.get(), pipeline.exe_name("sfa3"))
        if os.path.exists(exe):
            self.game = exe
            self.play_btn.config(state="normal")
            self.folder_btn.config(state="normal")
            self.status.config(text="Already installed. Convert again to rebuild it.")

    def toggle_log(self):
        if self.log_shown.get():
            self.log.pack(fill="both", expand=True, padx=12, pady=(0, 6))
        else:
            self.log.pack_forget()
        self.root.geometry("")  # grow / shrink to fit

    def add_log(self, text):
        self.log.config(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.config(state="disabled")

    # ---------------- converting ----------------

    def start(self):
        image, install = self.image.get().strip(), self.install.get().strip()
        if not image:
            return self.fail("Choose your disc image first.")
        if not os.path.exists(image):
            return self.fail("That disc image wasn't found.")
        try:
            os.makedirs(install, exist_ok=True)
        except OSError as e:
            return self.fail(f"Can't create the install folder ({e.strerror}).")
        free = shutil.disk_usage(install).free / 2**30
        if free < NEEDED_GB:
            return self.fail(f"Not enough free space: about {NEEDED_GB} GB is needed while converting "
                             f"({free:.1f} GB free).")
        for s in pipeline.STEPS:
            self.marks[s].config(text="○", foreground="")
        self.convert_btn.config(state="disabled")
        self.play_btn.config(state="disabled")
        self.bar.config(value=0)
        self.worker = threading.Thread(target=self.work, args=(image, install), daemon=True)
        self.worker.start()

    def work(self, image, install):
        try:
            out = pipeline.run(image, progress=lambda st, f, m: self.events.put(("progress", st, f, m)),
                               log=lambda m: self.events.put(("log", m)),
                               paths=pipeline.Paths(install=install), toolchain="zig")
            self.events.put(("done", out))
        except pipeline.SetupError as e:
            self.events.put(("error", str(e)))
        except Exception as e:  # anything unexpected: still tell the player
            self.events.put(("error", f"something went wrong: {e!r}"))

    def poll(self):
        try:
            while True:
                ev = self.events.get_nowait()
                if ev[0] == "progress":
                    _, st, frac, msg = ev
                    i = pipeline.STEPS.index(st)
                    for s in pipeline.STEPS[:i]:
                        self.marks[s].config(text="✔", foreground="#2a8a2a")
                    self.marks[st].config(text="►", foreground="#c07000")
                    self.bar.config(value=frac)
                    self.status.config(text=msg + (f"  {int(frac * 100)}%" if 0 < frac < 1 else ""))
                elif ev[0] == "log":
                    self.add_log(ev[1])
                elif ev[0] == "done":
                    for s in pipeline.STEPS:
                        self.marks[s].config(text="✔", foreground="#2a8a2a")
                    self.game = ev[1]
                    self.bar.config(value=1)
                    self.status.config(text="Done! Press Play.")
                    self.convert_btn.config(state="normal")
                    self.play_btn.config(state="normal")
                    self.folder_btn.config(state="normal")
                elif ev[0] == "error":
                    for s, m in self.marks.items():
                        if m.cget("text") == "►":
                            m.config(text="✖", foreground="#c02020")
                    self.convert_btn.config(state="normal")
                    self.fail(ev[1][:1].upper() + ev[1][1:])
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def fail(self, msg):
        self.status.config(text=msg, foreground="#c02020")
        self.add_log("error: " + msg)
        self.root.after(8000, lambda: self.status.config(foreground="#555"))

    # ---------------- after ----------------

    def play(self):
        if self.game and os.path.exists(self.game):
            subprocess.Popen([self.game], cwd=os.path.dirname(self.game))

    def open_folder(self):
        folder = self.install.get()
        if os.name == "nt":
            os.startfile(folder)  # noqa: S606 (opens Explorer)
        else:
            subprocess.Popen(["xdg-open", folder])


def convert_headless(image, install):
    """SFA3-Setup --convert IMAGE FOLDER: the same conversion without the window."""
    def show(step, frac, msg):
        print(f"\r[{pipeline.STEPS.index(step) + 1}/{len(pipeline.STEPS)}] {msg} {int(frac * 100)}%   ",
              end="", flush=True)
    try:
        out = pipeline.run(image, progress=show, log=lambda m: None, paths=pipeline.Paths(install=install),
                           toolchain="zig")
    except pipeline.SetupError as e:
        sys.exit(f"\nfailed: {e}")
    print(f"\ndone: {out}")


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--convert":
        return convert_headless(os.path.abspath(sys.argv[2]), os.path.abspath(sys.argv[3]))
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
