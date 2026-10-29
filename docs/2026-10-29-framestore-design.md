# Framestores, freeze and the camera-shutter effects: design

Status: implemented on branch plan5-framestores (2026-10-29); still to judge by eye: §6.
Reverse-engineering source: `../tools/re/FREEZE.md`, plus `../tools/re/FINDINGS.md` and `ALGO.md`.

## 1. Intent

The owner saw camera-shutter transitions play wrong, and suspected a missing "freeze" code. Reverse-engineering shows two separate things:

1. **No effect freezes the picture** (FREEZE.md §1, proven). The camera-shutter look of Photo Fly Out ! comes from tag 0x1B's per-pixel **mix level L**:
   - a full-screen white flash (frames 0–3);
   - then the photo of A fading in over thirds (frames 4–6) inside a white border;
   - then the print flying off, with a glare.

   The engine ignores L today, so it draws a white card.
2. **Freeze is the framestore panel.** DV1 and DV2 are two framestores. FREEZE/LIVE switches the digital store between following an input (live, 4 fields late) and holding stills. The grab buttons 1–4 capture an input into DV1. Framestore files load into DV1 or DV2. None of this exists yet: DV1/DV2 are black.

The owner wants the original's behaviour, including the 4-field digital lag ("copy the 4-field lag"), and Toaster-format framestore files when saving.

## 2. Goals

### A. Mix level L in the "!" DVEs

The seven "!" DVEs carry art planes and tag 0x1B (FINDINGS "DVE control codes" §3–4; FREEZE.md §2):
- Hand Pull !
- Photo Fly Out !
- Flip Out Bottom2 !
- Flip Chrome Edge !
- Infinite Video Space !
- Fly Over Crumpled Edge !
- Warp To Globe !

For these, each pixel's palette index gives:
- **S**, the routed source: 1, 3 or 9 the effect colour (BKG generator), 0 none;
- **L**, the level, 0–3.

Which bus S = 1 and S = 3 show (Ruling 10, a **GUESS**): S = 1 is the bus that is **not** moving, analog; S = 3 is the moving bus's picture as the move moves it (the DV path, delayed while live: the moving bus holds 0x30|input from the take, FREEZE.md §5). The routing descriptor (tag 0x80000007; ALGO.md §7, the bus permutations) says which bus moves:
- descriptors 3 and 32 move **A**: S = 1 is B, S = 3 is A's moved picture (Photo Fly Out ! and Warp To Globe ! are 3; Flip Out Bottom2 ! and Fly Over Crumpled Edge ! are 32);
- every other descriptor moves B: S = 1 is A, S = 3 is B (Hand Pull ! is 2; Flip Chrome Edge ! routes only S = 0; Infinite Video Space ! has no tag 0x1B).

The output is `mix(routed source S, DVE picture, L/3)`. L = 0 shows S alone and L = 3 shows the DVE picture. Where S = 0 (unused), the current art rule stays.

- Blend weights are thirds. **GUESS** (FREEZE.md §2): one place, easy to change.
- The CMAP high-nibble shading (Flip Chrome Edge !) is not modelled. The colour is the generator's.
- **Acceptance:** Photo Fly Out ! at Fast shows:
  - white on frames 0–3;
  - A at ⅓, ⅔ and full inside the print on frames 4, 5 and 6;
  - a white border ring afterwards.

  The other six still play, with no unknown-code pixels.

### B. The DV store in the engine (`Switcher`)

- **Stills and state.** The store holds two stills, DV1 and DV2, each one progressive 768×482 frame, plus a state of live or frozen. It starts live with both stills black.
- **Digital delay: 4 ticks** (fields), the owner's choice. A live digital picture shows its input as it was 4 ticks earlier. Until 4 ticks of history exist, the oldest available frame is shown. The lag applies to:
  - **digital moves:** the moving picture of DVE ANIMs and ALGO kinds 0 and 3, when the moving bus shows an input. The key-0 background stays current (analog), so a digital move starts with the lag's stutter, as on the original;
  - **live DV1/DV2 on a bus:** pressing DV1 or DV2 while live shows that bus's previously selected input through DV, 4 ticks late. This is the manual's "pre-select DV on Main" (FREEZE.md §3).
- **One DV input.** The original has one DV input mux, so the store has a single **DV input** (input 1–4). The last bus to press DV1/DV2 while live, or the last digital move, sets it to that bus's input. Every bus showing live DV shows that same delayed input. While live, DV1 and DV2 on a bus are one "live digital" state; the button lit is the one pressed.
- **Frozen.** DV1 and DV2 on a bus show their stills. A digital move whose moving bus shows a frozen DV moves that still, with no lag.
- **Take start with a frozen store** (FREEZE.md §5, proven: flag 0x200). If the store is frozen and no bus shows DV1 or DV2, a digital move (DVE ANIM, ALGO kind 0/3) switches the store to **live** at take start. The stills are kept and the FREEZE/LIVE lights follow. The moving picture is then the delayed input.
- **Freeze.** Switching to frozen copies the picture the live DV path is showing (the delayed DV input) into **one** still:
  - DV2 if a bus shows DV2;
  - otherwise DV1.

  The other still is kept. A bus that showed live DV keeps showing that DV, now the still. Switching back to live keeps both stills. They are overwritten only by a freeze, a grab or a load.
- **Grab N** (inputs 1–4):
  1. if the store is live, it freezes first;
  2. 4 ticks later, input N's current frame is written to **DV1**;
  3. the store stays frozen;
  4. DV1 is selected on Main (FREEZE.md §3: the MAIN DV1 node gets message 0xb).
- **One frame per still** (my call, since output is progressive): a still is one frame, not a 4-field colour frame. STABILIZE therefore has nothing to do (non-goal).
- **ChromaFX** keeps its current DV2 behaviour, with no lag (my call).
- **Store operations during effects** (my call; the DV memory is shared, spec 5.8):
  - FREEZE, LIVE, grab and SAVE are **refused** while a transition runs (T-bar above 0 or Auto) or a ChromaFX is engaged. The status bar gives the reason: "finish the effect first".
  - Loading a framestore **disengages** an engaged ChromaFX first, as the original does (FREEZE.md §4, step 1). It is refused during a running transition.
  - The original's re-grab into DV1 when a DVE starts with DV2 on a bus is not modelled (non-goal).

### C. Framestore files

- **Loading** the 75 shipped framestores (FREEZE.md §4). A file is a framestore when its CrUD says FRAM, or it has none and its sibling icon's does, or it has no CrUD anywhere and its picture is a DV one this loader accepts (Ruling 9: twelve LightWaveDemo pictures; `vt::isFramestore`). They are `FORM ILBM` files, 752×480 (CG frames 768×480), 16 planes, with a `PLTP` chunk mapping planes to two 8-bit byte lanes, 6 and 7.
  - **The 16-plane mapping, solved from `4.2/Framestore/ColorBars`** (FREEZE.md §6 item 4):
    - Lanes 6 and 7 hold **NTSC composite samples at 4 fsc**, in the DV's own levels. They are the `vt::DvQuad` samples (Y−I, Y−Q, Y+I, Y+Q) that `dv_colour.h` already decodes bit-exactly.
    - PLTP entry n is a (lane, bit) byte pair for plane n; a lane's value is the sum of its plane bits shifted by their bit numbers.
    - For file row r, if `((r + 1) >> 1) & 1` is set, lanes 6 and 7 trade places.
    - For pixel x, let p = x % 4. Lane 6 is quad sample p and lane 7 is sample (p + 2) % 4; the partner pixel x ^ 1 fills the other two samples the same way. `vt::dvQuadToRgb(quad)` gives the colour.
    - Check: ColorBars decodes to all seven 75 % bars within ±2 at rows 120 and 240, and column 0 is blanking (lane value 60).
  - The 24-plane RGB variant (lanes 8/9/10, LSB first) loads too.
  - A loaded still is placed in the 768×482 raster as FINDINGS ("DV framestore picture is 752 x 480") and the plan's task decide. Its placement is checked against ColorBars.
  - ColorBars settled the 16-plane mapping (P1; FREEZE.md §6 item 4), so the 16-plane files load; none is refused for its layout.
- **Loading behaviour** (FREEZE.md §4, proven):
  - clicking a framestore crouton loads it into **DV2**, or DV1 when the loader's rule says so (Main or Superimpose showing DV2, or Preview's saved source being DV2);
  - the store is frozen;
  - the target DV is selected on Preview.
- **Project grid.** The drawer picker also lists the framestore folders: `4.2/Framestore/...`, `4.2/CG/Frames` and the user's saved-framestore folder.
- **Saving (SAVE).** SAVE writes a Toaster framestore crouton:
  - `FORM ILBM`, 752×480, 24 planes, with a `PLTP` like the Switcher's own template at 0x11ca4c (RGB lanes);
  - an 80×50 icon made from the still;
  - `CrUD` with `TYPE FRAM`, readable by the loader above.

  It saves the DV shown on Preview, else on Main, else DV1. It goes to a user folder (default `Documents/vtoast/Framestore`, set in Settings) with a file dialog for the name. The original's "1 Field / 4 Fields" choice is not offered, since a still is one frame.

### D. The panel

- The existing panel nodes come alive: FREEZE, LIVE (mutually exclusive, lit by the state), grab 1–4, STABILIZE and SAVE (SCREEN.md; FREEZE.md §3).
- STABILIZE stays visible but does nothing (non-goal).
- Keyboard **f** freezes and **l** goes live.
- DV1 and DV2 on Main and Preview behave as in B.
- The status bar names a refused action, for example "SAVE: no still in DV1/DV2".

## 3. Non-goals

- STABILIZE's pixel operations (2 fields or 1 field), and SAVE "1 Field": stills are single progressive frames.
- 0x1B L levels on ANIM "!" effects other than the seven DVEs, the 4-bit level ramps of the Fade/Wipe "!" dissolves, and the CMAP high-nibble shading. These are later, if the owner reports one.
- CG page data inside `CG/Frames` files: they load as pictures only.
- The Superimpose bus, keys, Overlay, and LightWave's use of BKG.
- Saving to the 16-plane YIQ format.

## 4. Decided by me (please check)

- **L blend weights:** thirds, as a GUESS kept in `HardwareGuesses`.
- **Stills:** one progressive frame each, with no 4-field jitter; STABILIZE is inert.
- **Freeze target:** freeze writes one still (DV2 if a bus shows DV2, else DV1), and grab writes DV1 only (grab sets the DV1 select bit, FREEZE.md §3).
  - On the original, live mode keeps writing the store, so a loaded framestore is lost as soon as LIVE is pressed.
  - Here, stills survive LIVE, and a freeze replaces only one of them. This is kinder to your saved work.
  - Which frame a freeze keeps (the delayed DV picture) is a guess.
- **One DV input:** one live digital input at a time, as on the original's single DV mux.
- **Saved files:** structured like the original's (template 0x11ca4c: 24-plane ILBM, PLTP, icon, CrUD FRAM) and verified by round trip in vtoast. Whether the original Toaster opens them cannot be tested here.
- **ChromaFX:** no lag on ChromaFX's DV2 path.
- **SAVE source:** the DV on Preview, else Main, else DV1. The save folder is configurable.

## 5. Tests

- **L levels:** Photo Fly Out ! at its key frames against hand-computed pixels for white, ⅓·A, ⅔·A and A. The seven "!" DVEs render every frame with no unknown codes.
- **Delay:** with 4 numbered input frames, a digital move's moving picture at tick k shows input frame k − 4, clamped to the oldest. Live DV1 on Main shows the delayed input. A frozen DV on Main moves the still with no lag.
- **Freeze, live and grab:**
  - freeze copies the shown picture into DV1, or DV2 when a bus shows DV2, and keeps the other still;
  - a DVE taken with a frozen store and no DV on a bus switches the store to live and keeps the stills;
  - FREEZE, grab and SAVE are refused during a running transition;
  - loading a framestore disengages ChromaFX;
  - grab 3 writes input 3's frame into DV1 after 4 ticks, freezes and selects DV1 on Main;
  - live followed by freeze keeps the stills.
- **Framestore files:**
  - the ColorBars framestore decodes to the 75 % bars within ±2 per channel at the bar centres;
  - every shipped framestore loads;
  - save then load gives the same still pixel for pixel;
  - a saved file's CrUD type is FRAM.
- **App (offscreen):**
  - clicking FREEZE lights FREEZE and freezes;
  - grab 1 puts DV1 on Main;
  - clicking the ColorBars crouton in the Framestore drawer puts DV2 on Preview, showing bars;
  - SAVE writes a file that appears in the saved-framestore drawer.

## 6. For the owner, by eye

- Photo Fly Out ! and the other six "!" DVEs. The four that move A (Photo Fly Out !, Warp To Globe !, Flip Out Bottom2 !, Fly Over Crumpled Edge !) start on A and end on B (`tools/algo_contact.py --set bang`: A = colour bars, B = slide).
- The digital lag's stutter at the start of digital moves.
- The shipped framestores: colours and placement.
