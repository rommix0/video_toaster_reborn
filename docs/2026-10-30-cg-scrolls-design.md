# CG scroll and crawl pages on the Switcher — design

Date: 2026-10-30. Owner: "Yes, scrolls next" (questions skipped earlier); every call below is the controller's, marked where it is a guess.
Authority: `../tools/re/CGSCROLL.md` (the trace) and `../tools/re/cgscroll_ref.py` (page parser, line and crawl renderers, palette, per-field motion; verified bit-exact against switcher.main under emulation).

## 1. Intent

The second ToasterCG step plays the four **scroll and crawl pages** (`4.2/CG/Scrolls`: MojoWorld.Scroll, NightOfTerror; `4.2/CG/Crawls`: EBS.Crawl, Novel.Crawl) over Main as the original does: credits rolling up, a ticker crossing the screen, field-exact.

Non-goals: the ToasterCG editor, the Sequencer (and its late-start abort), the unreachable "repeat" loops, crawl render overruns ("too slow" stop), the CG book data (tag 0x69), project saving.

## 2. Behaviour (CGSCROLL.md; "proven"/"verified" unless marked)

### A. The page
- File: a flat `FORM ILBM` glyph strip (2 planes = 2-bit antialias coverage) + `FORM MVPG` (a raw C struct: header, glyph, placement and line records, CGSCROLL.md §1) + icons + `CrUD` TYPE `SCRO` / `CRAW`.
- Rendering is the original's own (CGSCROLL.md §2): scroll lines into a 640-wide picture, crawl windows into one long strip, outline/shadow per placement attribute, the crawl quirks (1 px per window seam, edge from coverage plane 1, XOR cut-out) reproduced. Palette: 0 = transparent, 1-3 face, 4 edge, 5/6 edge→face blends, 7 face, each cut to 12 bits (high nibble). Pre-render at load (builder's call how).

### B. Loading and playing
- New drawers **`CG/Scrolls`** and **`CG/Crawls`** (beside `CG/Keys`).
- **Click**: loads the page; **nothing on air, no bus change**. It is the loaded crouton, and the effect becomes the plain built-in Fade (Auto / Take / T-bar act as that Fade; **inferred**, CGSCROLL.md §3). Selecting another crouton drops it.
- **Double click**: load, then **play** (msg 0x21; double click is the inferred sender, as for KEYF).
- **Play start**: Main = BKG becomes input 1 (kept afterwards); FREEZE forced on (stays on afterwards); Program = **Main with the page keyed over it**, index 0 transparent (HARDWARE GUESS: mixer mode 1 + matte nibble 0xF). A Superimpose key is hidden while it plays (mode 1). Speed = the page's tag 0x8000006a; the speed buttons don't change it (faithful).
- **Scroll motion** (verified): s = 1..4 frame lines per field, s = 0 one line per frame (long fields only = every other tick); each line starts at the first field boundary at or after its y and after the previous line; field j shows fed row T[j−1] + Y − 480 on frame line Y, rows entering at the bottom (first visible line 475; lines 0..475 shown). One vtoast tick = one field.
- **Crawl motion** (verified): 2, 4, 8, 16 px per field for speed 0..3 (4 = 3); field j shows strip column x + v·j − 768 at raster column x; the band is frame lines 2·(y0 >> 1) + 44 + r; outside the band nothing is keyed.
- **Placement (HARDWARE GUESS)**: the crawl's columns are raster columns (trace). The scroll's 640 columns are **centred** (x0 = 64), not at the traced Switcher-bitmap column 98, because that bitmap's own registration with video is unknown and the CG centres its text; `HardwareGuesses::scrollX0`, owner judges by eye.
- **End, run once**: a scroll ends 500 rows after its last line; a crawl at field 768·(windows − 1)/v (EBS 8448, Novel 1248).
- **End, pause (scroll with tag 0x80000087 ≠ 0, NightOfTerror)**: stops with the last line just fed, holds tag >> 1 fields, then fades over 22 fields with levels 255, 243, … 3, then ends. Fade level → opacity L/255 (HARDWARE GUESS).
- **After the end**: Main alone on Program, a cut; FREEZE stays on; the page stays the loaded crouton (a double click plays it again).

### C. While it plays
- **Right mouse button** anywhere on the Switcher screen cuts it off at once (as the end). Not during the pause hold and fade (faithful: not polled there).
- Every other Switcher input (panel, keys, T-bar, croutons) is **ignored while a page plays** (the original never returns to its input loop), with a status-bar note "a scroll is playing: right-click stops it" (vtoast's addition). One choke point in the app, driven by the snapshot.
- The right-drag T-bar shortcut doesn't start while a page plays (the right button stops the page instead).

## 3. Shape (controller's calls; the builder pins names)
- **Engine**: `loadCgScroll(path)` → a page object (pre-rendered indexed picture/strip, palette, line feed table or the per-field position law, speed, pause, kind) beside `loadCgKeyPage`. The Switcher owns playback (playing, field counter, phase hold/fade), stepped by `tick()`, composited in `render()` allocation-free; snapshot gets `cgScroll` (loaded, playing). Every UI action is a `ProgramLoop::post/postChecked`.
- **Goldens**: `tools/make_cgscrolls_golden.py` → `tests/golden/cgscrolls.jsonl` from `cgscroll_ref.py`: per page, Program-frame CRC32s (Main = navy 0x1B2A80 solid) at fields covering the first rows, a line start, mid-run, the stop / hold / fade fields (NightOfTerror), the last field and after the end; plus end fields. The C++ matches exactly.
- **App**: the two drawers, double click plays, right button stops, input ignored while playing, status text.

## 4. Left to the builder
How to pre-render (whole page vs. windows), memory layout, test names, the exact status wording.

## 5. Follow-ups noted, not in this project
- The scroll x registration (64 vs 98) and the colour-0 key: owner's eye.
- The ToasterCG editor.
