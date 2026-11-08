"""Framestore contact sheets for the owner's by-eye check (Plan 5).

One sheet per drawer - 4.2/Framestore (and its subfolders), 4.2/CG/Frames and
4.2/Programs/Switcher_Support/StartupFrames - written to build/framestore_sheets/<drawer>.png: every
framestore in a grid at 1/4 size with its name. Each still is decoded by `vt_render <framestore>
--out <dir>` (visible area, 640x432). The ".I" icon files are skipped. Run from anywhere:
python -P -E tools/framestore_contact.py [vt_render.exe]
"""
import subprocess, sys, tempfile
from pathlib import Path
from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parent.parent
DATA = REPO / "data" / "4.2"
EXE = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "build" / "msvc" / "cli" / "vt_render.exe"
OUT = REPO / "build" / "framestore_sheets"
COLS, W, H = 4, 160, 108  # a 640x432 still at 1/4 size

# (sheet name, folder). The Framestore folder itself is "Framestore"; each subfolder is its own drawer.
DRAWERS = [("Framestore", DATA / "Framestore")]
DRAWERS += [(d.name, d) for d in sorted((DATA / "Framestore").iterdir()) if d.is_dir()]
DRAWERS += [("CG_Frames", DATA / "CG" / "Frames"), ("StartupFrames", DATA / "Programs" / "Switcher_Support" / "StartupFrames")]

def sheet(name, folder, tmp):
    files = sorted(f for f in folder.iterdir() if f.is_file() and f.suffix.lower() != ".i")
    cells = []
    for f in files:
        frames = tmp / name / f.name
        r = subprocess.run([str(EXE), str(f), "--out", str(frames)], capture_output=True, text=True)
        if r.returncode:
            print(f"{name}/{f.name}: skipped: {r.stdout}{r.stderr}".strip())
            continue
        cells.append((f.name, Image.open(frames / "still.png").resize((W, H), Image.LANCZOS)))
    if not cells:
        print(f"{name}: no framestores")
        return
    rows = (len(cells) + COLS - 1) // COLS
    img = Image.new("RGB", (COLS * W, rows * H), "black")
    d = ImageDraw.Draw(img)
    for i, (fname, pic) in enumerate(cells):
        x, y = i % COLS * W, i // COLS * H
        img.paste(pic, (x, y))
        d.text((x + 3, y + 2), fname, fill="white", stroke_width=1, stroke_fill="black")
    img.save(OUT / f"{name}.png")
    print(f"{name}: {len(cells)} framestores -> {OUT / (name + '.png')}")

OUT.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory() as t:
    for name, folder in DRAWERS:
        sheet(name, folder, Path(t))
