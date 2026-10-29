"""Key contact sheets for the owner's by-eye check (Plan 6, design 6).

Written to build/key_sheets/ by the vt_render CLI. A = 75 % bars on Main, the key card (black, a
white disc, a grey ramp, eight grey steps) on Superimpose or Preview, B = the crimson slide:
  Key law.png       the key alone (no transition): Key Black and Key White at L 0..257
  Art Card Key.png  the card loaded, T-bar 0..511 (L = T-bar / 2): Main keyed over Preview
  <move>.png        one Auto with a key up: 7 frames over the transition, then the end state
Run from anywhere: python -P -E tools/key_contact.py [vt_render.exe]
"""
import re, subprocess, sys, tempfile
from pathlib import Path
from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parent.parent
FX = REPO.parent / "4.2" / "Effects"
EXE = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "build" / "msvc" / "cli" / "vt_render.exe"
OUT = REPO / "build" / "key_sheets"
W, H = 320, 216
FADE = FX / "2000FX/Smooth Fade"
CARD = FX / "4000FX/Control/ART CARD KEY"
KEYED = ["--a", "bars", "--c", "card"]  # the card on Superimpose over bars
FLY_ON = ["--a", "bars", "--b", "card"]  # the card on Preview flies on

# (sheet name, crouton, vt_render options)
MOVES = [
    ("Smooth Fade (drops the key)", "2000FX/Smooth Fade", KEYED + ["--key", "black"]),
    ("Push On L-R (fly on, Black)", "4000FX/Push-Pull/Push On L-R", FLY_ON + ["--key", "black"]),
    ("Push On L-R (fly on, White)", "4000FX/Push-Pull/Push On L-R", FLY_ON + ["--key", "white"]),
    ("Split Corners In (fly on, DVE)", "2000FX/Blinds/Split Corners In", FLY_ON + ["--key", "black"]),
    ("Expand Fade In (fly on, fade move)", "2000FX/Digital/Expand Fade In", FLY_ON + ["--key", "black"]),
    ("Blinds 3 Shrink (fly off)", "2000FX/Blinds/Blinds 3 Shrink", KEYED + ["--key", "black"]),
    ("Infinite Tiles (two-phase)", "2000FX/Digital/Infinite Tiles", KEYED + ["--key", "black"]),
]

def render(effect, opts, out):
    r = subprocess.run([str(EXE), str(effect), "--out", str(out)] + opts, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"vt_render {effect.name} {' '.join(opts)} failed: {r.stdout}{r.stderr}")
    return int(re.match(r"rendered (\d+) frames", r.stdout).group(1))

def grid(name, cells, cols):
    """cells: (ppm path, label) in reading order."""
    rows = (len(cells) + cols - 1) // cols
    img = Image.new("RGB", (cols * W, rows * H), "black")
    d = ImageDraw.Draw(img)
    for i, (ppm, label) in enumerate(cells):
        x, y = i % cols * W, i // cols * H
        img.paste(Image.open(ppm).resize((W, H), Image.LANCZOS), (x, y))
        d.text((x + 4, y + 3), label, fill="white", stroke_width=1, stroke_fill="black")
    img.save(OUT / f"{name}.png")
    print(f"{name} -> {OUT / (name + '.png')}")

def key_law(tmp):
    cells = []
    for key in ("black", "white"):
        for level in (0, 1, 64, 128, 192, 256, 257):
            out = tmp / f"law_{key}_{level}"
            render(FADE, KEYED + ["--key", key, "--level", str(level), "--tbar", "0"], out)
            v = "card" if level == 0 else "Main" if level == 257 else f"v {(level if key == 'black' else 257 - level) - 1}"
            cells.append((out / "0001.ppm", f"Key {key.title()}  L {level}  ({v})"))
    grid("Key law", cells, 7)

def art_card(tmp):
    cells = []
    for key in ("black", "white"):
        for t in range(0, 512, 73):
            out = tmp / f"card_{key}_{t}"
            render(CARD, KEYED + ["--key", key, "--tbar", str(t)], out)
            level = t // 2
            cells.append((out / "0001.ppm", f"Art Card {key.title()}  T-bar {t}  L {level}  v {level if key == 'black' else 255 - level}"))
    grid("Art Card Key", cells, 8)

def move(name, rel, opts, tmp):
    out = tmp / name
    count = render(FX / rel, opts + ["--after", "1"], out)
    during = count - 1
    cells = [(out / f"{k:04d}.ppm", f"{name}  {k}/{during}")
             for k in (round(i * (during - 1) / 6) + 1 for i in range(7))]
    cells.append((out / f"{count:04d}.ppm", f"{name}  after the take"))
    grid(name, cells, 4)

OUT.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory() as t:
    tmp = Path(t)
    key_law(tmp)
    art_card(tmp)
    for name, rel, opts in MOVES:
        move(name, rel, opts, tmp)
