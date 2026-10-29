"""ALGO contact sheets for the owner's by-eye check (Plan 4).

Renders each named crouton with the vt_render CLI (one Auto transition, A = colour bars,
B = slide) and writes build/algo_sheets/<name>.png: 8 frames evenly spaced over the transition,
left to right, top to bottom. Run from anywhere: python -P -E tools/algo_contact.py [--set bang] [vt_render.exe]
--set bang renders the seven "!" DVEs (tag 0x1B mix levels, Plan 5) instead of the ALGO list.
"""
import argparse, re, shutil, subprocess, sys, tempfile
from pathlib import Path
from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parent.parent
FX = REPO.parent / "4.2" / "Effects" / "4000FX"
ap = argparse.ArgumentParser()
ap.add_argument("exe", nargs="?", default=REPO / "build" / "msvc" / "cli" / "vt_render.exe")
ap.add_argument("--set", choices=["bang"], help="render this named set instead of the default list")
args = ap.parse_args()
EXE = Path(args.exe)
OUT = REPO / "build" / "algo_sheets"

# (sheet name, path under 4000FX). The border crouton carries tag 0x80000064 (ALGO.md 3).
EFFECTS = [
    ("Crop To Center", "Wipes/Crop To Center"),
    ("Squeeze T-B", "Digital/Squeeze T-B"),
    ("Push On L-R", "Push-Pull/Push On L-R"),
    ("Slats Slip On", "Blinds/Slats Slip On"),
    ("Mosaic Transition", "Digital/Mosaic Transition"),
    ("Wipe 3 Slots In", "Wipes/Wipe 3 Slots In"),
    ("Smooth Fade", "Smooth Fade"),
    ("Dip To Matte", "Dissolves/Dip To Matte"),
    ("Push On Border L-R", "Push-Pull/Push On Border L-R"),
]
# The seven "!" DVEs (FREEZE.md 2), in the design's order.
BANG = [
    ("Hand Pull !", "Digital/Hand Pull !"),
    ("Photo Fly Out !", "Trajectory/Photo Fly Out !"),
    ("Flip Out Bottom2 !", "Trajectory/Flip Out Bottom2 !"),
    ("Flip Chrome Edge !", "Digital/Flip Chrome Edge !"),
    ("Infinite Video Space !", "Space/Infinite Video Space !"),
    ("Fly Over Crumpled Edge !", "Digital/Fly Over Crumpled Edge !"),
    ("Warp To Globe !", "Digital/Warp To Globe !"),
]
N, COLS, W, H = 8, 4, 320, 216

def sheet(name, rel, tmp):
    frames = tmp / name
    r = subprocess.run([str(EXE), str(FX / rel), "--out", str(frames), "--a", "bars", "--b", "slide"],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{name}: vt_render failed: {r.stdout}{r.stderr}")
    count = int(re.match(r"rendered (\d+) frames", r.stdout).group(1))
    img = Image.new("RGB", (COLS * W, (N // COLS) * H), "black")
    d = ImageDraw.Draw(img)
    for i in range(N):
        k = round(i * (count - 1) / (N - 1)) + 1
        x, y = i % COLS * W, i // COLS * H
        img.paste(Image.open(frames / f"{k:04d}.ppm").resize((W, H), Image.LANCZOS), (x, y))
        d.text((x + 4, y + 3), f"{name}  {k}/{count}", fill="white", stroke_width=1, stroke_fill="black")
    img.save(OUT / f"{name}.png")
    print(f"{name}: {count} frames -> {OUT / (name + '.png')}")

OUT.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory() as t:
    for name, rel in (BANG if args.set == "bang" else EFFECTS):
        sheet(name, rel, Path(t))
