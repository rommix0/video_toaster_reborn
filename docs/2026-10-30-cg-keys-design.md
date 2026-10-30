# CG key pages on the Switcher — design

Date: 2026-10-30. Owner said "proceed" and skipped the questions; every call below is the controller's, marked where it is a guess.
Authority: `../tools/re/CGKEY.md` (the trace) and `../tools/re/cgkey_ref.py` (dissolve law verified against the original under emulation; page decoder).

## 1. Intent

ToasterCG's pages already hold their finished pictures. The first ToasterCG step plays the six **key pages** (`4.2/CG/Keys`: 4StarCinema, Feature, KeyPage1, KeyPage2, LowerThird1, LowerThird2) on the Switcher as the original does: click to load, Auto to dissolve the page on over Main, Auto again to dissolve it off. It closes the Switcher's "CG key pages" loose end (keys design §3 non-goal).

Non-goals (later projects): scroll and crawl pages (glyph strip + `MVPG`, still to be decoded), the ToasterCG editor (text, PostScript fonts, attributes), Books, the Sequencer, the 4-bit alpha mode (mode 5 / 0x124196, unused by the files), project saving. CG framestore pages already load (Plan 5).

## 2. Behaviour (CGKEY.md; "proven" unless marked)

### A. The page
- File: flat `FORM ILBM` 768 × 400, BMHD y = 43, 18 planes: PLTP lanes 6/7 (composite, as a framestore) and lane 0x0b bits 1-0 = **alpha a ∈ 0..3**. Then icons and `CrUD` TYPE `KEYF`.
- The fill goes into a DV at raster lines y + kFramestoreY0 … (44..443); x from 0 (BMHD x ignored). The rest of that DV is black (rows outside the page: black is the builder's call; CGKEY.md unknown 5).
- **Colour phase follows the DV line, not the file row**: lanes 6/7 swap when `((line + 1) >> 1) & 1` with line = file row + y. For a framestore (y = 0) this is today's rule, so framestores don't change.
- The alpha is kept **apart from the DV** (the original keeps it on the Amiga display): the Switcher holds the page's 768 × 482 alpha plane. LIVE overwriting the DV does not touch it (faithful).

### B. Loading
- A KEYF crouton is listed in a new **`CG/Keys`** drawer (next to `CG/Frames`).
- **Click**: the fill goes into DV1 or DV2 by the framestore rule (the existing FRAM handler's choice), the store freezes, the alpha is installed. **Nothing goes on air**: no bus or keyer change. The Superimpose DV1/DV2 button shows a small "CG" mark (0x15066; builder draws a plain "CG" label if the original art isn't extractable).
- Clicking the page that is already loaded puts its DV on Preview.
- **Double click** (guess, CGKEY.md unknown 4): load, then cut on.
- The page is the loaded crouton: selecting any other crouton (effect, framestore, control) clears the page (alpha gone, mark gone; the DV keeps the picture).

### C. On air
- Superimpose = the page's DV (fill), Main = background, luma keyer forced OFF and shown OFF while the page is up.
- **Mix (HARDWARE GUESS, `HardwareGuesses`)**: for dissolve level L (0..255), q = L >> 6, pixel alpha a: a = 0 → 0; else code = a + q − 3, mix = clamp(64·code + (L & 63), 0, 255). Output = Main + (fill − Main)·mix/255. At L = 255: a = 1/2/3 → 50/75/100 %.

### D. Auto, T-bar, reverse
- **Auto** with the page off dissolves it on; the next Auto dissolves it off. N = speed >> 2 steps; levels L_k = (k·⌊0xff80/N⌋) >> 8 for k = 0..N−1, then 255; off is the mirror ending at 0 (cgkey_ref.py, verified).
- **Speed** = the crouton's resolved speed (tag 0x80000032 / Speed tag; no page has one, so crouton.library's default: 60 → 15 steps + 1). The speed buttons change it as for effects (builder: match how ALGO timing resolves).
- **Step time (HARDWARE GUESS)**: 4 fields per step, so a dissolve lasts ≈ speed fields (60 → 1 s). In `HardwareGuesses`.
- **T-bar**: level L = T-bar / 2 (0 at rest = off, 511 = full), live, both ways, as the Art Card Key's 2L link.
- **Reverse Auto** flips direction only with the T-bar part-way.
- **Original bugs**: an Auto from a part-way T-bar at or below ⌈511/N⌉ divides by zero on the Amiga → we **don't crash**: that Auto runs one step to the end (ruling). The wrong step count of an on-Auto from part-way is reproduced (cheap, visible only as pace).
- **End of the off-dissolve**: a Take-style cut empties Superimpose; Main kept; Preview gets the page DV only if Preview == Main; the loaded crouton becomes the plain built-in Fade; the picture stays in the DV, the alpha leaves.

### E. Drops while the page is up
- OFF / Black / White (and `o` `[` `]`): the page cuts off, the keyer shows OFF. The clip level does nothing to the page.
- Another Superimpose source: the page cuts off and Superimpose is left **empty** (the pressed source is discarded). The page's own DV on Superimpose **cuts it on**.
- Take with the page up: a normal cut that drops it. Take with the page not up: Preview → Main **and** the page cuts on.
- f / l keys refused while a page is loaded; the FREEZE/LIVE panel buttons are not intercepted (faithful).
- After a drop that leaves the page loaded, Auto doesn't bring it back; its DV on Superimpose or reloading does.

## 3. Shape (controller's calls; the plan pins names)
- **Engine**: `loadCgKeyPage(path) → {Frame fill, alpha plane}` beside `loadFramestore` (shares PLTP lane gathering and the composite decode, which gains a line offset). The Switcher owns the page state (loaded DV, alpha, on/off, level, direction) next to the key state, rendered by the mixer as a third key mode; the snapshot gets `cgPage` (loaded DV or none, up, level) for the panel. Every UI action is a `ProgramLoop::post/postChecked` (loop-thread rule).
- **Goldens**: a script in `tools/` writes, from `cgkey_ref.py`, CRC32s of the decoded fill and alpha for all six pages and the level sequences for a few speeds, plus mixed-frame CRCs at a few levels (A = navy 0x1B2A80 Main, as the other goldens). The C++ matches exactly.
- **App**: the `CG/Keys` drawer, the "CG" mark, status text for refusals, double click.

## 4. Left to the builder
Black outside the page rows; the CG mark's pixels; how the speed buttons resolve a page's speed (match ALGO croutons); test names.

## 5. Follow-ups noted, not in this project
- CGKEY.md suggests 752-wide framestores start at x = 0, not centred at x = 8 as `framestoreRect` guesses. Owner to judge by eye later.
- Scroll and crawl pages; then the ToasterCG editor.
