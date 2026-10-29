# Framestores, freeze and the camera-shutter effects: Implementation Plan (Plan 5)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** DV1/DV2 become real framestores (live with a 4-field lag, or frozen stills; grab, load and save), and the seven "!" DVEs mix by tag 0x1B's level bits, so Photo Fly Out ! flashes white and fades the photo in.

**Architecture:**
- **Engine.**
  - `dve.cpp` gains the tag 0x1B route/level mix.
  - A new `framestore.h/.cpp` reads and writes FRAM croutons.
  - `Switcher` owns the DV store: two stills, a live/frozen state, one DV input, and a short history ring per input for the digital delay.
  - Effects that move a picture digitally get the delayed picture through `RenderCtx`.
- **App.** The panel's framestore nodes, the drawer picker and Settings drive it all through `ProgramLoop` posts, as today.

**Tech Stack:** C++20 (MSVC 2022, Ninja), Qt 6.8.3, Catch2 (amalgamated), Python 3 with numpy and PIL for the tools.

**Spec:** `docs/2026-10-29-framestore-design.md` (approved 2026-10-28). Reverse-engineering sources:
- `../tools/re/FREEZE.md`, binding for the § rules cited below;
- `../tools/re/FINDINGS.md` ("DVE control codes", "Which colour an effect's matte shows").

## Facts settled while writing this plan (binding)

### P1. The 16-plane framestore format (it was FREEZE.md §6 item 4)

- The file's lanes 6 and 7 hold **NTSC composite samples at 4 fsc**, in the DV's own levels. They are exactly the `vt::DvQuad` samples (Y−I, Y−Q, Y+I, Y+Q) that `dv_colour.h` already decodes bit-exactly.
- **Lanes.** PLTP entry n is a byte pair (lane, bit) for plane n. Lane value = Σ plane bit << bit.
- **Row phase swap.** For file row r, if `((r + 1) >> 1) & 1` is set, lanes 6 and 7 trade places.
- **Quad for pixel x.** Let p = x % 4. After the swap:
  - lane 6 at x is quad sample p, and lane 7 at x is sample (p + 2) % 4;
  - the partner pixel x ^ 1 fills the other two samples the same way.
- **Colour.** `vt::dvQuadToRgb(quad)` gives the colour.
- **Check.** `4.2/Framestore/ColorBars` decodes this way to all seven 75 % bars within ±2 at rows 120 and 240.
  - x = 53 + 107·k gives white 190/190/190, yellow 191/191/0, cyan 0/190/189, green 0/190/0, magenta 191/0/190, red 192/0/0 and blue 0/0/189.
  - Column 0 is blanking (lane value 60).

### P2. The 24-plane framestore format

- The PLTP puts planes 0–7 in lane 8 (bits 0–7), planes 8–15 in lane 9 and planes 16–23 in lane 10, LSB first.
- Lane 8 is R, 9 is G and 10 is B: the ordinary 24-bit ILBM order (`4.2/Images/jun-y/Image1.IFF` is 752×480 like this).

### P3. A framestore crouton file

`4.2/Framestore/ColorBars` (229 580 bytes) is a run of top-level FORMs:
1. `FORM ILBM`: the picture (BMHD, PLTP, BODY).
2. `FORM ILBM`: icon 1, 80×50, 6 planes, a 64-entry CMAP, uncompressed BODY.
3. `FORM " PUS"`: one chunk `ILBM`, length 4, holding the previous FORM's size field.
4. `FORM ILBM`: icon 2, 80×50, 2 planes, CMAP 000000 606060 909090 d0d0d0, uncompressed.
5. `FORM " PUS"`: the same kind of chunk, for icon 2.
6. `FORM CrUD`: the 72 bytes in Task 3.
7. `FORM " PUS"`: chunk `CrUD`, length 4, value 0x40.

The sibling `ColorBars.I` holds forms 2–7 only.

The Switcher's own save template (switcher.main 0x11ca4c) has this BMHD: 752×480, x 0, y 0, 24 planes, masking 0, compression 1, pad 0, transparent 0, aspect 10:11, page 752×480. Its PLTP is as in P2.

### P4. Tag 0x1B in the "!" DVEs

The data layout is in `vt::paletteRoutes`' comment. For the seven "!" DVEs, one row, first index 0.
- **Mask per entry.** Each entry long e picks mask `masks[(e >> 28) & 7]` (FINDINGS "Which colour…" 3). The Switcher merges `(cmapBits & ~mask) | (e & mask)`.
- **Bit layout.** In the merged low word:
  - the source bits are 0x604;
  - L is bits 0–1, which in the CMAP is `cmap[i].b & 3`.
- **The seven tables:**

  | effect | mask | S | L |
  |---|---|---|---|
  | Hand Pull ! | 0x80000714 | S from the table | from the CMAP |
  | Photo Fly Out ! | 0x80000717 | S 1 (planes 0–3) / 9 (plane 4) | planes 2–3 |
  | Flip Out Bottom2 ! | 0x80000607 | from the table | from the table |
  | Fly Over Crumpled Edge ! | 0x80000717 / 0x113 | from the table | from the table |
  | Warp To Globe ! | 0x80000717 | from the table | from the table |
  | Flip Chrome Edge ! | 0x113 | all S = 0 (art rule) | — |
  | Infinite Video Space ! | none | no tag 0x1B (unchanged) | — |

- **Display bitmap.** The Switcher takes the frame's last plane out as the sprite (FREEZE.md §1 item 6). The palette index is therefore planes 0..count−2, bit p = plane p.

## Global Constraints

- C++20, MSVC 2022, Qt 6.8.3, Catch2 amalgamated, FFmpeg as already vendored. **No new dependencies.** Python tools run with `python -P -E`.
- Build and test with `tools\build.bat` (configure, build, `ctest -LE live`). Every task ends with the full suite green: `vt_tests`, the app tests and the CLI tests.
- Engine timing: one tick is one 59.94p frame (one Toaster field). Render then tick (`switcher.h` header comment).
- Every hardware guess lives in `vt::HardwareGuesses` (engine/include/vt/hardware_guesses.h) with a `HARDWARE GUESS` comment. New ones:
  - `dveLevelMix` (thirds);
  - `digitalDelay` (4).
- Frames are 768×482 RGBA (`vt::Frame`).
- A framestore still sits in the raster at x0 = (768 − w) / 2 and y0 = 1. This is a GUESS: `kFramestoreY0 = 1` in framestore.h, and the x formula. Everything outside it is black.
- **Downloads** need the owner's explicit permission; none are needed here.
- **No input injection.** Never drive the owner's desktop with synthetic mouse or keyboard input; the app tests run offscreen.
- **No microphone.** Never record the owner's microphone in tests.
- **Refusal texts**, which the status bar shows verbatim:
  - `"finish the effect first"`: FREEZE, LIVE, grab or SAVE during a transition or an engaged ChromaFX;
  - `"a transition is running: finish it first"`: a load during a running transition;
  - `"SAVE: no still in DV1/DV2"`: SAVE with no still yet;
  - `"not a framestore: <reason>"`: a file that cannot load.
- Match the surrounding code: comment density, names, idiom. Comments say why, and cite FREEZE.md § where they port a finding.

## Review Focus

1. **Unreadable framestore files.**
   - Cases: a truncated BODY, an unsupported plane count such as the 18-plane CG key files, or a crouton that is not FRAM.
   - Expected: `loadFramestore` returns nullopt with a reason, the status bar shows it, and nothing crashes.
   - Task 2 adds `framestore refuses bad files`.
2. **Grab of an input with no picture** (a null `SourceFrames` entry).
   - Expected: DV1 becomes black, and there is no crash or stale picture.
   - Task 4 adds `grab of a missing input gives black`.
3. **Save paths.**
   - Cases: the folder does not exist yet, or the name has spaces or non-ASCII characters.
   - Expected: the folder is created, the round trip works, and `.I` is written beside the file.
   - Task 3 adds `framestore save creates the folder and handles unicode names`.
4. **Freeze or grab right after start-up, before any history exists.**
   - Expected: the still is black (or the oldest frame), never uninitialised memory.
   - Task 4 adds `freeze before any render gives black`.
5. **Loading while a bus already shows DV1 or DV2, live or frozen.**
   - Expected: live DV is stripped, the FREEZE.md §4 target rule picks the other DV, and Preview shows it.
   - Task 4 adds `load target follows the loader rule`.

---

### Task 1: Tag 0x1B level mix in the "!" DVEs

**Files:**
- Modify: `engine/include/vt/dve.h`, `engine/src/dve.cpp`, `engine/src/effect_dve.cpp`, `engine/src/effect_detail.h` (the `makeDveEffect` signature), `engine/src/effect_factory.cpp`, `engine/include/vt/hardware_guesses.h`
- Test: `tests/test_dve.cpp`, `tests/test_effects.cpp`

**Interfaces:**
- Produces, in `dve.h`:
  ```cpp
  // Tag 0x1000001B for one frame of a "!" DVE (P4): per palette index of the display bitmap,
  // source 0 = none (the art rule), 1 = A, 3 = B, 9 = the colour generator; level L 0..3.
  struct DveRoute { uint8_t source = 0, level = 0; };
  std::vector<DveRoute> dveRoutes(const std::vector<uint8_t>& tag1b, const std::vector<Rgb>& cmap);
  struct DveMix {
      const std::vector<DveRoute>* routes = nullptr;
      const Frame* a = nullptr;          // S = 1 (the analog bus picture, never delayed)
      const Frame* b = nullptr;          // S = 3
      Rgba8 colour{0, 0, 0, 255};        // S = 9
      const Frame* generator = nullptr;  // S = 9 with a Snow generator
  };
  int renderDve(const DveControl&, const Frame& moving, const Frame& background, const std::vector<Rgb>& cmap,
                Frame& out, const HardwareGuesses&, const DveMix* mix = nullptr);
  ```
- Produces: `DveControl::index` (`std::vector<uint8_t>`, 241×768 line-major). It is the display-bitmap palette index: every plane except the one the line table came from, bit i taken from the i-th of those planes in order. It is filled only when `Planes::count > 3`, and empty otherwise.
- Produces: `HardwareGuesses::dveLevelMix`, a `std::array<uint8_t, 4>` defaulting to `{0, 85, 170, 255}`. It is the DVE picture's share for L = 0..3, a GUESS (FREEZE.md §2).
- Produces: `makeDveEffect(CroutonBasics, const Anim&, const std::vector<uint8_t>& tag1b, std::string* why)`. An empty tag1b means no routes.

- [ ] **Step 1: Write the failing tests** in `tests/test_dve.cpp`.

  `dveRoutes reads source and level through the mask`:
  - Build a one-row table: rows 1, step 1, first 0, count 4, masks[0] = 0x80000714, masks[1] = 0x80000717. Entries:
    - 0x00009003 (mask 0, S 9);
    - 0x00001002 (mask 0, S 1);
    - 0x10003001 (mask 1, S 3, L 1);
    - 0x00000003 (mask 0, S 0).
  - Give a CMAP whose index 0 has b = 0x02 and index 1 has b = 0x01.
  - Expect `{9, 2}`, `{1, 1}`, `{3, 1}`, `{0, *}`. Mask 0 has no L bits, so L comes from the CMAP; mask 1 has them, so L comes from the entry.
  - With masks[0] = 0x00000113 (no source bits), entry 0x00009003 gives source 0.
  - A table shorter than its count gives an empty vector.

  `renderDve mixes a routed pixel by its level`:
  - Use a `DveControl` with every key 255 − (c + 3) and line[k] = k.
  - `index` has 1 at one pixel and 0 everywhere else. Routes are `{ {1,0}, {9,2} }`. `moving` is solid (200,100,0); `a` is solid (10,20,30); the colour is (255,255,255).
  - Expect:
    - the index-0 pixel equals a (L = 0 shows S alone);
    - the index-1 pixel equals `(255*(255-170) + d*170 + 127) / 255` per channel, with d being the same pixel rendered without `mix`.
  - The default `dveLevelMix` is asserted to be `{0, 85, 170, 255}`.

  `decodeDve fills the display index only for "!" DVEs`:
  - A 3-plane control frame gives an empty index.
  - A 6-plane frame with the signature on planes 2 and 5 gives an index whose bit 2 is plane 2 and bit 4 is plane 4. Plane 5 is not part of the index.

- [ ] **Step 2: Write the failing acceptance test** in `tests/test_effects.cpp`, `Photo Fly Out ! flashes white, then fades A in`.
  - Load `4.2/Effects/4000FX/Trajectory/Photo Fly Out !` and render frames 0–10 with `ctx.animFrame = k`.
    - a = `ramp(1)`, b = `ramp(2)`.
    - `ctx.background` is white (255,255,255,255), `generator` is null and `guesses` is the defaults.
  - At pixel (384, 240), read the control frame's `index`: `loadCrouton` → `readAnim` → `decodeDve(an.planes[k]).index[120 * 768 + 384]`. Assert:
    - plane 4 is set in frames 0–6;
    - L (bits 2–3) is 0 in frames 0–3, then 1, 2 and 3 in frames 4, 5 and 6.
  - Assert the output pixel:
    - frames 0–3: exactly white;
    - frames 4–6: `(255*(255-w) + d*w + 127) / 255` with w = 85, 170 and 255, where d is that frame rendered through `renderDve` without `mix`.
  - Frame 10 has at least 1000 pixels whose index has plane 4 set and L = 0, and all of them are white (the border ring).

  Also add `the seven "!" DVEs render without unknown codes`. Every frame of each of the seven, rendered through `loadEffect`, has no (128,128,128) pixel inside `kVisibleArea`. Use a = solid (10,20,30), b = solid (40,50,60) and a white background, so no blend can make that grey.

- [ ] **Step 3: Run them and check they fail.**
  Run: `tools\build.bat -R vt_tests` (then `build\msvc\tests\vt_tests.exe "dveRoutes*"` and so on).
  Expected: compile errors for `dveRoutes` / `DveMix` / `index`.

- [ ] **Step 4: Implement.**
  - **`dveRoutes`.** Parse as `paletteRoutes` does, row 0 only. The result is sized first + count. For entry i:
    - source = S when `(mask & 0x604) == 0x604` and S ∈ {1, 3, 9}, else 0;
    - L = `((e & mask) | ((i < cmap.size() ? cmap[i].b : 0) & ~mask)) & 3`.
  - **`decodeDve`.** Builds `index` when count > 3.
  - **`renderDve`.** When `mix && mix->routes && !c.index.empty()`, take i = `index[k*768 + x]`. If i < routes size and source != 0:
    - s is a / b / generator-or-colour at (x, y);
    - d is the pixel before the art layer;
    - out = `(s*(255-w) + d*w + 127)/255` per channel with w = `dveLevelMix[L]`, alpha 255.
    - Otherwise use today's art rule.
  - **`DveEffect`.** Computes routes per frame from that frame's palette (`palettes_[k]`) at construction and passes a `DveMix` with a, b, `ctx.background` and `ctx.generator`.
  - **Factory.** Passes `c->blobs[kTagSourceWords]` if present.

- [ ] **Step 5: Run the full suite.** Run: `tools\build.bat`. Expected: all pass.
- [ ] **Step 6: Commit.**
  `git commit -m "feat(engine): tag 0x1B level mix in the ! DVEs (Photo Fly Out ! flash and fade)"`

### Task 2: Reading framestore files

**Files:**
- Create: `engine/include/vt/framestore.h`, `engine/src/framestore.cpp` (add to `engine/CMakeLists.txt`)
- Modify: `engine/include/vt/crouton.h`, `engine/src/crouton.cpp`, `engine/src/effect_factory.cpp`, `../tools/re/FREEZE.md` (§4 lanes paragraph and §6 item 4: the solved mapping, with P1's numbers)
- Test: `tests/test_framestore.cpp` (add to `tests/CMakeLists.txt`)

**Interfaces:**
- Produces: `CroutonType::Fram`, from TYPE `FRAM`. `loadEffect` refuses it with `"a framestore (load it into DV1/DV2), not an effect"`.
- Produces, in `framestore.h`:
  ```cpp
  inline constexpr int kFramestoreW = 752, kFramestoreH = 480;
  inline constexpr int kFramestoreY0 = 1;  // HARDWARE GUESS: raster line of file row 0
  // Where a w x h still sits in the raster: x0 = (kRasterW - w) / 2, y0 = kFramestoreY0.
  Rect framestoreRect(int w = kFramestoreW, int h = kFramestoreH);
  // A FRAM crouton's picture as a raster frame (black outside framestoreRect). 16 planes with PLTP
  // lanes 6/7: composite (P1); 24 planes with lanes 8/9/10: RGB (P2). Anything else, or a
  // malformed file: nullopt and *whyNot.
  std::optional<Frame> loadFramestore(const std::filesystem::path&, std::string* whyNot = nullptr);
  ```
- Accepted sizes: w ≤ 768 and h ≤ 480, both even. CG/Frames are 768×480.

- [ ] **Step 1: Write the failing tests.**

  `ColorBars decodes to the 75 % bars`:
  - Load `4.2/Framestore/ColorBars`.
  - For k = 0..6, at raster (61 + 107k, 121) and (61 + 107k, 241), expect within ±2 per channel: (191,191,191), (191,191,0), (0,191,191), (0,191,0), (191,0,191), (191,0,0), (0,0,191).
  - (0, 0) and (767, 481) are black. `framestoreRect()` is {8, 1, 752, 480}.

  `every shipped framestore loads`:
  - Walk `4.2/Framestore`, `4.2/CG/Frames`, `4.2/Programs/Switcher_Support/StartupFrames` and `4.2/3rdparty`.
  - Every non-icon file whose crouton type is Fram loads. Count them and require ≥ 60 (FREEZE.md §4: 63).
  - If any is refused, print its reason; the test fails.

  `framestore refuses bad files` (Review Focus 1):
  - a copy of ColorBars cut to 100 000 bytes;
  - an ANIM effect (`Hand Pull !`);
  - a missing path.

  Each returns nullopt with a non-empty reason.

  `loadEffect refuses a framestore`: ColorBars gives nullptr, with the reason above.

- [ ] **Step 2: Run them and check they fail.** Run: `tools\build.bat -R vt_tests`. Expected: framestore.h not found.
- [ ] **Step 3: Implement** with `loadCrouton` → the first `FORM ILBM` → `readBitmapHeader` + `bodyToPlanes`.
  - Find PLTP with `formChunks`; it has n words, one per plane.
  - Composite decode as in P1, through `dvQuadToRgb`.
  - No PLTP and 24 planes counts as RGB.
  - Refuse 16 planes without lanes 6/7.
  - Then edit FREEZE.md §4 and §6.4 to record P1 as proven (data: ColorBars).
- [ ] **Step 4: Run the full suite.** Run: `tools\build.bat`. Expected: all pass.
- [ ] **Step 5: Commit.**
  `git commit -m "feat(engine): read framestore croutons (16-plane composite, 24-plane RGB)"`

### Task 3: Writing framestore files

**Files:**
- Modify: `engine/include/vt/framestore.h`, `engine/src/framestore.cpp`, `engine/include/vt/ilbm.h`, `engine/src/ilbm.cpp`
- Test: `tests/test_framestore.cpp`, `tests/test_ilbm.cpp`

**Interfaces:**
- Produces in `ilbm.h`: `std::vector<uint8_t> packByteRun1(std::span<const uint8_t> row);`. It packs one row: literal runs ≤ 128 and repeat runs of 3 to 128.
- Produces in `framestore.h`:
  ```cpp
  // Writes `path` (picture + icons + CrUD, P3) and `path` + ".I" (icons + CrUD), creating the
  // folder. The picture is `still` inside framestoreRect(), 24-plane RGB (P2) with the 0x11ca4c
  // BMHD (P3). False and *whyNot on failure; no half-written file is left behind.
  bool saveFramestore(const std::filesystem::path&, const Frame& still, std::string* whyNot = nullptr);
  ```
- **Icon 1:** 80×50 and 6 planes. The CMAP is the 4×4×4 cube: index = r4·16 + g4·4 + b4, with levels 0, 85, 170, 255. Each icon pixel is the box average of its block of the still's rect, with x from 9.4·i and y from 9.6·j, taking the nearest cube colour.
- **Icon 2:** 2 planes, CMAP 000000 606060 909090 d0d0d0, with the nearest grey of the block's Rec.601 luma.
- **CrUD form, verbatim** (72 bytes):
  `46 4F 52 4D 00 00 00 40 43 72 55 44 54 59 50 45 00 00 00 08 46 52 41 4D 00 00 00 00 4C 49 42 53 00 00 00 18 FF FF FC D0 00 10 65 66 66 65 63 74 73 2E 6C 69 62 72 61 72 79 00 00 00 54 41 47 53 00 00 00 04 00 00 00 00`

- [ ] **Step 1: Write the failing tests.**
  - `packByteRun1 round trips` (test_ilbm): for rows of 0, 1, 2, 127, 128, 129 and 300 bytes of random, constant and mixed content, `unpackByteRun1(packByteRun1(r), r.size()) == r`.
  - `framestore save then load is exact`:
    - a still with `ramp(3)` inside `framestoreRect()` and black outside;
    - `saveFramestore` to a temp dir, then `loadFramestore` gives the same frame, pixel for pixel;
    - `loadCrouton(path)->type == CroutonType::Fram`;
    - the `.I` file exists and `vt::assets::scanCroutons(dir)` returns one crouton with an 80×50 image.
  - `saved file has the original's layout`:
    - The top-level forms are ILBM, ILBM, " PUS", ILBM, " PUS", CrUD, " PUS".
    - The picture BMHD bytes equal `02 F0 01 E0 00 00 00 00 18 00 01 00 00 00 0A 0B 02 F0 01 E0`, and the PLTP is 48 bytes as in P2.
  - `framestore save creates the folder and handles unicode names` (Review Focus 3): save to `<temp>/new folder/Bärs ✓` and load it back.
- [ ] **Step 2: Run them and check they fail.** Run: `tools\build.bat -R vt_tests`. Expected: undefined `saveFramestore` / `packByteRun1`.
- [ ] **Step 3: Implement.**
  - Interleave rows as ILBM BODY requires (row y: plane 0..23), each row packed separately.
  - Write to `<path>.tmp` and `<path>.I.tmp`, then rename both. On failure, remove the temps.
  - `vt_assets` is not an engine dependency. The scanCroutons check lives in the test only; link `vt_tests` against `vt_assets` if it is not already.
- [ ] **Step 4: Run the full suite.** Run: `tools\build.bat`. Expected: all pass.
- [ ] **Step 5: Commit.**
  `git commit -m "feat(engine): save framestore croutons like the original's (24-plane, icons, CrUD FRAM)"`

### Task 4: The DV store in the Switcher

**Files:**
- Modify: `engine/include/vt/switcher.h`, `engine/src/switcher.cpp`, `engine/include/vt/hardware_guesses.h`
- Test: `tests/test_switcher.cpp`

**Interfaces:**
- Produces: `HardwareGuesses::digitalDelay`, an int defaulting to 4. It is the ticks a live digital picture lags its input, clamped to 0..8 where used. (The owner chose to copy the lag; the length is a GUESS, FREEZE.md §3.)
- Produces, in `Switcher`:
  ```cpp
  // --- DV store: DV1 and DV2 (design B) ---
  bool frozen() const;
  int dvInput() const;                      // 0..3, the one DV input (starts 0)
  const Frame& still(Source dv) const;      // DV1 or DV2 (black until written)
  // FREEZE (true) / LIVE (false). Refused with "finish the effect first" while
  // transitionActive() && !overlayEngaged(). Freezing writes the live DV picture (dvInput's
  // delayed frame) into DV2 if any bus shows DV2, else DV1.
  bool setFrozen(bool frozen, std::string* whyNot = nullptr);
  // Grab input 0..3 into DV1: freezes if live, then digitalDelay ticks later the input's frame
  // goes into DV1 and Main selects DV1 (FREEZE.md 3). Same refusal as setFrozen.
  bool grab(int input, std::string* whyNot = nullptr);
  bool grabPending() const;
  // A framestore loaded (FREEZE.md 4): refused with "a transition is running: finish it first"
  // while auto or the T-bar > 0 with nothing engaged; disengages an engaged ChromaFX; buses
  // showing live DV go back to In(dvInput); target = DV2 if Main or Overlay shows DV1, DV1 if
  // either shows DV2, else DV1 if Preview shows DV2, else DV2; the still is written there, the
  // store frozen, the target selected on Preview.
  bool loadStill(const Frame& still, std::string* whyNot = nullptr);
  // SAVE's source: the DV on Preview, else on Main, else DV1. nullopt (and "SAVE: no still in
  // DV1/DV2") if neither still was ever written.
  std::optional<Source> saveSource(std::string* whyNot = nullptr) const;
  ```
- Behaviour changes:
  - **`select(bus, DV1|DV2)` while live** sets `dvInput_` from that bus's previous selection when it was an input.
  - **`sourceFrame(DV1|DV2)`.**
    - Frozen: that still.
    - Live: the delayed frame of `dvInput_`. That is the newest recorded frame with tick ≤ now − delay, else the oldest recorded one, else black.
  - **`render`.**
    1. First, if a grab is due, copy that input's current frame (black for a null input) into DV1 and clear the grab.
    2. Then record `dvInput_`'s current frame in its ring (size delay + 1, frames allocated on first use).
    3. Then compose as before.
  - **`tick`.** Counts the grab's wait down. When it reaches 0, it selects DV1 on Main.
  - `renderSource` reads the history but never records.

- [ ] **Step 1: Write the failing tests** with numbered inputs: `solid({uint8_t(n), 0, 0, 255})` for tick n.
  - `live DV on a bus lags its input by 4 ticks`: Main In1, then select(Main, DV1) while live. Render tick n with input 1 = frame n:
    - `dvInput() == 0`;
    - program = frame max(n − 4, first recorded);
    - tick 0..3 all show frame 0.
  - `freeze copies the delayed picture into DV1, or DV2 when a bus shows DV2`, in two Switchers.
    - First: Preview DV2 live, setFrozen(true) after 6 ticks. `still(DV2)` = frame 2 (6 − 4), and `still(DV1)` stays black.
    - Second: no DV on a bus. The freeze writes DV1.
  - `live then freeze keeps the other still`.
  - `freeze, live and grab are refused during a transition`: setTBar(100), then each returns false with "finish the effect first".
  - `grab 3 writes input 3 after 4 ticks, freezes and selects DV1 on Main`:
    - grab(2) at tick t; ticks t..t+3 do not change still(DV1) from the freeze's picture;
    - the render at t+4 gives still(DV1) = input 3's frame at t+4;
    - selected(Main) == DV1 and frozen().
  - `grab of a missing input gives black` (Review Focus 2).
  - `freeze before any render gives black` (Review Focus 4).
  - `load target follows the loader rule` (Review Focus 5). The five cases of FREEZE.md §4 step 2:
    - Main DV1 → DV2;
    - Overlay DV2 → DV1;
    - Preview DV2 → DV1;
    - nothing → DV2;
    - Main live DV1 → stripped to In1 first, then DV2.

    Each ends with frozen() and Preview = target.
  - `loading a framestore disengages ChromaFX`: engage a ChromaFX (setTBar(511) on a CRFX crouton), then loadStill → true, chromaEngaged() false.
  - `saveSource order`: Preview DV2 → DV2; Main DV1 → DV1; neither → DV1; no still ever written → nullopt with the text.

- [ ] **Step 2: Run them and check they fail.** Run: `tools\build.bat -R vt_tests`. Expected: missing members.
- [ ] **Step 3: Implement.**
  - Replace `Frame dv_` with `std::array<Frame, 2> stills_` and `bool written_[2]`.
  - Add `mutable std::array<std::vector<Frame>, 4> hist_` with matching ticks.
  - Keep `render` const; the history, the stills a grab writes and the grab state are `mutable`.
  - Comment the history as the one exception to "render reads only".
- [ ] **Step 4: Run the full suite.** Run: `tools\build.bat`. Expected: all pass. Existing DV1/DV2 tests that expected black may need the black-until-written reading.
- [ ] **Step 5: Commit.**
  `git commit -m "feat(engine): DV1/DV2 framestores: live with a 4-field lag, freeze, grab, load"`

### Task 5: Digital moves go through the DV store

**Files:**
- Modify: `engine/include/vt/effect.h`, `engine/include/vt/switcher.h`, `engine/src/switcher.cpp`, `engine/src/effect_dve.cpp`, `engine/src/effect_algo.cpp`
- Test: `tests/test_switcher.cpp`, `tests/test_effects.cpp`

**Interfaces:**
- Consumes: Task 4's store and its history.
- Produces, in `effect.h`:
  ```cpp
  // Which bus a digital move moves first (FREEZE.md 5): None for everything but the DVE moves
  // (role MovingB -> B, else A) and ALGO kinds 0/3 (descriptor 2 -> B, else A). ChromaFX: None.
  enum class DigitalMove { None, A, B };
  virtual DigitalMove digitalMove() const { return DigitalMove::None; }
  ```
  and in `RenderCtx`:
  ```cpp
  // The DV path's picture of A and B: the bus's input delayed by HardwareGuesses::digitalDelay
  // while the store is live and the bus shows an input; null otherwise (use a / b: a frozen DV
  // still, BKG, live DV). Digital moves move these; their key-0 background and tag 0x1B routes
  // keep a / b (analog).
  const Frame* digitalA = nullptr;
  const Frame* digitalB = nullptr;
  ```
- Switcher behaviour:
  - **Recording.** While `effect_->digitalMove() != None`, render also records the inputs selected on Main and Preview.
  - **Take start.** `beginTransitionIfIdle`, when the effect's digitalMove() is not None:
    1. if the store is frozen and no bus shows DV1 or DV2, it goes live (stills kept);
    2. then, if live and the moving bus (A → Main, B → Preview) shows an input, `dvInput_` becomes that input.
  - **`render`.** Sets `ctx.digitalA` / `ctx.digitalB` as defined above, only for a digital-move effect.
- Effects:
  - **DveEffect.** The moving frame is `ctx.digitalA ? *ctx.digitalA : a` (B likewise), for every role. The background stays a / b or black.
  - **AlgoEffect kinds 0/3.** Likewise for `moving`. `background`, including desc 5's, stays analog.

- [ ] **Step 1: Write the failing tests.**
  - `a digital move's moving picture lags 4 ticks`:
    - Use `4.2/Effects/2000FX/Digital/Block Out`, a plain DVE ANIM. The test asserts `dveInfo` gives MovingA, so A moves.
    - Main In1 = solid frame n per tick, Preview In2 solid (9,9,9).
    - Select the effect at tick 0, run 10 ticks idle, then setTBar(1).
    - At a pixel whose control key (from `decodeDve` of frame 0) is ≥ 64, the program's red channel is current − 4. At a key-0 pixel it is In2.
  - `a frozen DV on the moving bus moves the still without lag`: Main DV1 frozen with a still. The moving picture equals the still on the first frame.
  - `a digital move taken with a frozen store and no DV on a bus goes live and keeps the stills`. Also, a cut and a dissolve leave the store frozen.
  - `digitalMove by role`: MovingB DVE → B; an ALGO desc-2 move → B; desc 3 → A; Dissolve and ChromaFX → None.
  - `ChromaFX has no lag`: an engaged ChromaFX over a changing Preview input shows the current frame.
- [ ] **Step 2: Run them and check they fail.** Run: `tools\build.bat -R vt_tests`. Expected: missing `digitalMove` / `digitalA`.
- [ ] **Step 3: Implement** as in the Interfaces block.
- [ ] **Step 4: Run the full suite.** Run: `tools\build.bat`. Expected: all pass. ALGO goldens are untouched, since they test the VM, not pictures.
- [ ] **Step 5: Commit.**
  `git commit -m "feat(engine): digital moves move the delayed DV picture; frozen store goes live at take start"`

### Task 6: The framestore panel

**Files:**
- Modify: `media/include/vt/media/monitor.h`, `media/src/program_loop.cpp`, `app/src/controller.h`, `app/src/controller.cpp`, `app/src/toaster_screen.h`, `app/src/toaster_screen.cpp`
- Test: `tests/app/test_toaster_screen.cpp`, `tests/app/test_support.h` (the fake actions), `tests/app/test_app_e2e.cpp`

**Interfaces:**
- Produces: `SwitcherSnapshot::frozen` (bool), set in `ProgramLoop::publish`.
- Produces, in `SwitcherActions`:
  ```cpp
  virtual void setFrozen(bool frozen) = 0;  // FREEZE / LIVE buttons, keys f / l
  virtual void grab(int input) = 0;         // grab buttons 1-4: 0..3
  virtual void saveFramestore() = 0;        // SAVE (Task 7 fills in the app's behaviour)
  ```
- **Panel.**
  - New `Control` kinds: Freeze, Live, Grab (with `input`) and Save.
  - Nodes `fs_freeze`, `fs_live`, `fs_1`..`fs_4` and `fs_save` leave `kStubs`. `fs_stabilize` stays a Stub, so it reports "STABILIZE: not in version 1".
  - `lookOf`: FREEZE shows its selected look when `state_.frozen`, LIVE when not. The grab buttons and SAVE are momentary (pressed look only).
- **Keys.** `f` calls setFrozen(true) and `l` calls setFrozen(false), in `switcherKey`.
- **AppController.**
  - `setFrozen` and `grab` are `postChecked`, so refusals reach the status bar.
  - `saveFramestore()` is a stub here: `message(tr("SAVE: …"))` until Task 7.

- [ ] **Step 1: Write the failing tests.**
  - test_toaster_screen:
    - a click on `fs_freeze`'s rect calls `setFrozen(true)`; `fs_live` calls `setFrozen(false)`; `fs_3` calls `grab(2)`; `fs_save` calls `saveFramestore()`;
    - with `state.frozen = true`, FREEZE's look is 1 and LIVE's is 0, and the reverse when false;
    - keys `f` and `l` map to setFrozen;
    - `fs_stabilize` still goes to `notImplemented("STABILIZE")`.
  - test_app_e2e (offscreen, Offline mode): clicking FREEZE lights FREEZE, and `loop().switcherState().frozen` is true after `runOffline(1)`.
  - test_app_e2e: grab 1 puts DV1 on Main after `runOffline(6)`.
- [ ] **Step 2: Run them and check they fail.** Run: `tools\build.bat -R app`. Expected: missing members.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run the full suite.** Run: `tools\build.bat`. Expected: all pass.
- [ ] **Step 5: Commit.**
  `git commit -m "feat(app): FREEZE, LIVE and grab on the framestore panel; keys f and l"`

### Task 7: Framestore files in the app

**Files:**
- Modify: `app/src/project_grid.h`, `app/src/project_grid.cpp`, `app/src/controller.cpp`, `app/src/settings.h`, `app/src/settings.cpp`
- Test: `tests/app/test_project_grid.cpp`, `tests/app/test_settings.cpp`, `tests/app/test_app_e2e.cpp`

**Interfaces:**
- Consumes: `vt::loadFramestore`, `vt::saveFramestore` (Tasks 2–3), `Switcher::loadStill`, `saveSource` and `still` (Task 4).
- Produces: `AppSettings::framestorePath`.
  - Default: `QStandardPaths::DocumentsLocation` + `/vtoast/Framestore`.
  - INI key `framestores/path`.
  - It is edited on the Recording page as "Framestore folder:" (`folderRow`, object name `framestorePath`).
- Produces:
  ```cpp
  // Drawers: those under <data>/4.2/Effects (labelled as today), then <data>/4.2/Framestore
  // ("Framestore", "Framestore/CGDemo", ...), <data>/4.2/CG/Frames ("CG/Frames"), and the saved
  // folder ("Saved framestores", listed even when empty or missing).
  ProjectView(const std::filesystem::path& data42, const std::filesystem::path& savedFolder,
              ToasterScreen*, QWidget* parent = nullptr);
  void setSavedFolder(const std::filesystem::path&);
  void rescan();  // re-reads the drawer shown
  ```
  and in `AppController`, `bool saveFramestore(const QString& path)`. It is public, and the tests call it.
- **Loading.** In `AppController::loadCrouton`, when `vt::loadCrouton(c.effect)` is Fram:
  - `vt::loadFramestore`;
  - on failure, the message `"not a framestore: " + why`;
  - otherwise `postChecked(sw.loadStill(...))`.

  A double click does not run Auto, and nothing is cached.
- **Saving.** `saveFramestore()` (the SAVE button) asks with `QFileDialog::getSaveFileName` in `framestorePath`, creating it, then calls `saveFramestore(path)`. That posts a `postChecked` which:
  1. refuses with "finish the effect first" while `sw.transitionActive() && !sw.overlayEngaged()`, then takes `saveSource`;
  2. copies the still;
  3. calls `vt::saveFramestore` on the loop thread;
  4. queues `view_->rescan()` and a "Saved <name>" message back to the UI thread.

  This needs a `ponytail:` comment: it writes on the loop thread, so a frame may drop on SAVE; it moves to a worker if that shows.

- [ ] **Step 1: Write the failing tests.**
  - test_project_grid: the picker lists "Framestore", "CG/Frames" and "Saved framestores" after the Effects drawers. "Saved framestores" is listed for a folder that does not exist.
  - test_settings: framestorePath round trips through save/load, and the default ends with `vtoast/Framestore`.
  - test_app_e2e: choose "Framestore", click the ColorBars crouton, then `runOffline(1)`:
    - Preview is DV2 and frozen;
    - the Preview picture at raster (61, 121) is within ±2 of (191,191,191), and at (61 + 107·1, 121) of (191,191,0).
  - test_app_e2e:
    - `saveFramestore(<temp>/Saved/Test1)`, `runOffline(1)`, then process events;
    - `<temp>/Saved/Test1` and `Test1.I` exist;
    - with "Saved framestores" chosen (`framestorePath = <temp>/Saved`), the grid has one crouton named "Test1".
  - test_app_e2e: SAVE with no still shows "SAVE: no still in DV1/DV2". SAVE with the T-bar at 100 shows "finish the effect first" and writes no file.
- [ ] **Step 2: Run them and check they fail.** Run: `tools\build.bat -R app`. Expected: constructor mismatch / missing members.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run the full suite.** Run: `tools\build.bat`. Expected: all pass.
- [ ] **Step 5: Commit.**
  `git commit -m "feat(app): framestore drawers, loading into DV1/DV2 and SAVE"`

### Task 8: Sheets for the owner, and the docs

**Files:**
- Modify: `cli/vt_render.cpp` (a FRAM path writes the decoded still as `still.png` in `--out`), `tools/algo_contact.py` (an optional set name)
- Create: `tools/framestore_contact.py`
- Modify: `docs/2026-10-29-framestore-design.md` (status line), `tests/test_cli.cmake` (the FRAM case)

**Interfaces:**
- `vt_render <FRAM crouton> --out <dir>` writes `<dir>/still.png`: the full 768×482 raster, or the visible area unless `--underscan`. It exits 0, and exits 1 with the reason for a bad file.
- `python -P -E tools/algo_contact.py --set bang` writes `build/algo_sheets/<name>.png` for the seven "!" DVEs, using the existing `sheet()`.
- `python -P -E tools/framestore_contact.py` writes `build/framestore_sheets/<drawer>.png`: every framestore of a drawer in a grid at 1/4 size, with names. It needs the vt_render exe.

- [ ] **Step 1: Write the failing CLI test** in `tests/test_cli.cmake`. `vt_render 4.2/Framestore/ColorBars --out <tmp>` exits 0 and writes `still.png`.
- [ ] **Step 2: Run it and check it fails.** Run: `tools\build.bat -R cli`. Expected: FAIL.
- [ ] **Step 3: Implement** the CLI case and both tools. Run the tools. They must produce the sheets without errors.
- [ ] **Step 4: Update the design's status line** to "implemented on branch plan5-framestores (date); still to judge by eye: §6", and add P1 to its §2.C as the solved mapping.
- [ ] **Step 5: Run the full suite.** Run: `tools\build.bat`. Expected: all pass.
- [ ] **Step 6: Commit.**
  `git commit -m "tools: framestore and ! DVE contact sheets for the owner's check"`
