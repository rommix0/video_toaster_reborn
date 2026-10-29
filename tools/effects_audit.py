"""Effects library audit: render every crouton under 4.2/Effects once and flag the suspicious ones.

Each effect runs one Auto with A = the navy slide on Main and B = the crimson slide on Preview
(vt_render --stride 4 --after 1). Neither slide has black in it, so black in a frame is the effect's
own. Flags, per effect:
  refused   vt_render could not load or play it (the reason is kept)
  black     some frame is more than 10 % near-black (r, g, b < 16)
  start     the first frame is not A
  end       after the take, Program is not B (ordinary transitions only)
  still     no frame differs from the first (nothing moves)
ChromaFX and Overlays-drawer effects stay engaged: the start/end checks do not apply to them.
Byte-identical files are rendered once. Writes build/effects_audit/report.csv and one 8-frame strip
per flagged effect in build/effects_audit/flagged/. Run: python -P -E tools/effects_audit.py
"""
import csv, hashlib, re, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parent.parent
FX = REPO.parent / "4.2" / "Effects"
EXE = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "build" / "msvc" / "cli" / "vt_render.exe"
OUT = REPO / "build" / "effects_audit"
W, H = 320, 216

def load(ppm):
    return np.asarray(Image.open(ppm).convert("RGB"), dtype=np.int16)

def render(path, out, extra=()):
    r = subprocess.run([str(EXE), str(path), "--out", str(out), "--stride", "4", "--after", "1", *extra],
                       capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()

def engaged(rel):
    parts = [p.lower() for p in rel.parts]
    return "chromafx" in parts or "overlays" in parts

def audit(path, tmp, ref_a, ref_b):
    rel = path.relative_to(FX)
    out = tmp / hashlib.sha1(str(rel).encode()).hexdigest()
    rc, text = render(path, out)
    row = {"effect": rel.as_posix(), "flags": "", "frames": "", "max_black_%": "", "note": ""}
    if rc:
        row.update(flags="refused", note=text.splitlines()[-1] if text else "no reason")
        return row, []
    if "still.png" in text:
        row.update(note="framestore, not an effect")
        return row, []
    count = int(re.match(r"rendered (\d+) frames", text).group(1))
    frames = sorted(out.glob("*.ppm"))
    pics = [load(f) for f in frames]
    black = [float(np.mean(np.all(p < 16, axis=2))) * 100 for p in pics]
    flags = []
    if max(black) > 10: flags.append("black")
    if not engaged(rel):
        if not np.array_equal(pics[0], ref_a): flags.append("start")
        if not np.array_equal(pics[-1], ref_b): flags.append("end")
    if all(np.array_equal(p, pics[0]) for p in pics[1:]): flags.append("still")
    row.update(flags=" ".join(flags), frames=count, **{"max_black_%": f"{max(black):.1f}"})
    return row, frames

def strip(row, frames):
    pick = [frames[round(i * (len(frames) - 1) / 7)] for i in range(8)] if len(frames) >= 8 else frames
    img = Image.new("RGB", (4 * W, 2 * H), "black")
    d = ImageDraw.Draw(img)
    for i, f in enumerate(pick):
        x, y = i % 4 * W, i // 4 * H
        img.paste(Image.open(f).resize((W, H), Image.LANCZOS), (x, y))
        d.text((x + 4, y + 3), f"{f.stem}", fill="white", stroke_width=1, stroke_fill="black")
    d.text((4, 2 * H - 14), f"{row['effect']}  [{row['flags']}]", fill="yellow", stroke_width=1, stroke_fill="black")
    img.save(OUT / "flagged" / (row["effect"].replace("/", "__") + ".png"))

def main():
    (OUT / "flagged").mkdir(parents=True, exist_ok=True)
    for old in (OUT / "flagged").glob("*.png"): old.unlink()
    files = sorted(f for f in FX.rglob("*") if f.is_file() and f.suffix.lower() not in (".i", ".info"))
    first = {}  # content hash -> first path with it
    unique, dupes = [], {}
    for f in files:
        h = hashlib.sha1(f.read_bytes()).hexdigest()
        if h in first: dupes[f] = first[h]
        else: first[h] = f; unique.append(f)
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        rc, text = render(FX / "2000FX/Smooth Fade", tmp / "ref")
        if rc: sys.exit(f"reference dissolve failed: {text}")
        refs = sorted((tmp / "ref").glob("*.ppm"))
        ref_a, ref_b = load(refs[0]), load(refs[-1])
        rows = {}
        def one(f):
            row, frames = audit(f, tmp, ref_a, ref_b)
            if row["flags"]: strip(row, frames) if frames else None
            for p in frames: p.unlink()
            return f, row
        with ThreadPoolExecutor(4) as pool:
            for n, (f, row) in enumerate(pool.map(one, unique), 1):
                rows[f] = row
                if n % 50 == 0: print(f"{n}/{len(unique)}", flush=True)
    with open(OUT / "report.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, ["effect", "flags", "frames", "max_black_%", "note", "same_as"])
        w.writeheader()
        for f in files:
            row = dict(rows[dupes[f]]) if f in dupes else rows[f]
            row["effect"] = f.relative_to(FX).as_posix()
            row["same_as"] = dupes[f].relative_to(FX).as_posix() if f in dupes else ""
            w.writerow(row)
    counts = {}
    for row in rows.values():
        for flag in row["flags"].split() or ["ok"]: counts[flag] = counts.get(flag, 0) + 1
    print(f"{len(files)} files, {len(unique)} unique: {counts} -> {OUT / 'report.csv'}")

main()
