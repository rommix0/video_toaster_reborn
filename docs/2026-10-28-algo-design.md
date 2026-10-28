# ALGO Switcher moves: design

Status: implemented on branch plan4-algo (2026-10-28); still to judge by eye: the registration offset, wipe polarity, the fade-move mix, borders.
Reverse-engineering source: `../tools/re/ALGO.md`.
Python reference: `../tools/re/algo_ref.py`; it matches the original 68000 code byte for byte under emulation (`algo_check.py`: 112 programs, 118,160 steps, no mismatches).

## 1. Intent

The Wipes, Digital and Transitions drawers are full of ALGO croutons that the app refuses today ("Switcher move (ALGO) not reverse-engineered yet"). There are 462 of them, of 115 distinct programs: Crop To Center, Squeeze, Push On, Fly In, Slats, Mosaic, Infinite Tiles, Wipe 3 Slots In, Smooth Fade, Dip To Matte and the rest. The owner wants them to play as the Toaster played them. As with the DVE moves, the moves must be bit-exact: the per-step control tables must equal the original's.

An ALGO crouton holds no picture. It holds a small bytecode program (tag `0x1000001C`) for an animation VM. At each step the VM turns the effect position t into one of these:
- **Digital moves (kinds 0 and 3):** a DVE control row and line table, the same data the DVE ANIMs carry.
- **Wipes (kinds 1 and 4):** a 1-bit span row and a vertical band.
- **Fades (kind 2):** levels.

## 2. Goals

1. **The VM.** A C++ port of `algo_ref.Algo`, the program decoder plus opcodes 2–14 and 17, with the same 16.16 arithmetic. It must be byte-exact against golden data dumped from `algo_ref`.
2. **Digital moves (kind 0, 380 croutons).** The VM's H row (192 cells), V table (241 lines) and line range become a `DveControl` (ALGO.md §5.3). That control frame is drawn by the existing `vt::renderDve`, so DVE and ALGO share one display path.
   - **Borders.** The 26 border croutons (tag 0x64) draw their border in the Effect Color.
     - The Effect Color is one of the 8 matte colours, in BKG-button order. Snow never appears in a border.
     - Left and right borders are key codes 16..23.
     - Top and bottom borders are lines whose source line is 0xfc, which the Switcher fills with the colour. `DveControl`/`renderDve` gain this colour-line case (ALGO.md §5.4, §15).
   - **Roles** come from the routing descriptor (tag 0x80000007; ALGO.md §5.5, §15). Bus roles are a strong guess; the switch points are proven.

     | descriptor | moving picture | key 0 (background) |
     |---|---|---|
     | 2 | B | A |
     | 3 | A | B |
     | 5 | A, then B | A, then B |
     | 6 (Flip Matte, flag 8) | A, then B | BKG matte |

     The switch happens at T-bar 256. In Auto it happens at the first step with t ≥ 0x8000, or at the fold when flag 8 is set.
   - **State within a take** (proven, ALGO.md §15). The original keeps the H and V buffers from one step to the next. A fresh evaluation per t differs in about 24 programs, by stale edge entries. The port therefore keeps that state too:
     - **Reset:** at a take's first step (the first T-bar or Auto step after the effect is idle). The program is reinstalled, H is filled with the background value and V with 0xee/0xef.
     - **Kept:** H and V across every later step of the take, in the order the steps happened. Moving the T-bar back resets nothing.
     - **End of take:** T-bar 511 (bus swap), T-bar 0 (aborted), Auto finished, or a new crouton selected.
     - **Consequence:** `Effect` gains take-aware rendering for ALGO. The plan decides the mechanism (e.g. mutable per-take state with steps applied in order, and repeated renders of the same step returning the same frame). Auto is deterministic; a T-bar drag depends on every position the Switcher rendered.
   - **Fade moves.** On Expand Fade In and Squeeze Fade Out, the channel-23 level cross-fades the background with the moving composite (polarity as in goal 4).
3. **Wipes (kind 1, 56 croutons).**
   - The span row covers hires pixels 0..735 and the band covers frame lines 0..480 (ALGO.md §6).
   - Set bits show B and clear bits show A.
   - Flag 0x80 inverts the drawing.
4. **Fades (kind 2, 6 croutons).** The level runs 0..255. Level 0 shows the background input and 255 shows the second input. This polarity is a guess, but it is the only one under which every fade starts and ends on the right picture.
   - Smooth Fade mixes A to B by the level curve.
   - Dip To Matte goes A → matte, then matte → B (descriptor 8). The matte colour comes from tag 0x23 (Black), like the effect colour of other croutons.
5. **Positionable moves (kinds 3 and 4, 16 croutons).** They use the kind-0 and kind-1 renderers at their stored position. All 16 shipped files have stop position 0, so Auto runs to t = 1 (proven).
6. **Timing.**
   - Speed tags 0x57/0x58/0x59/0x56 give N, a count of fields, so N ticks.
   - T-bar: t = (pos << 16) / 511.
   - Auto step k (1..N): t = k · floor(0x10000 / N).
   - Flag 8 (there and back) folds t; flag 4 reverses it.
   - The take completes at the last step, as it does for the other effects.

## 3. Non-goals

These wait for later work:
- **Art Card Key** (kind 5, 4 croutons): it has no program and needs keys. It stays refused, with its own reason.
- **Interactive positioning** of kinds 3 and 4 (dragging the anchor with the mouse), including the held, open take that a non-zero stop position causes.
- **The four deep-colour Bar stills** (Bar Craters, Deco, Gold Bevel, Gold Thin). They run in the same VM (ALGO.md §11), but nobody has traced how their 10- and 12-plane pictures combine with A and B, or what channels 26 and 27 do. They stay refused as they are today.
- **Opcodes 15, 16 and 18–20:** no shipped program uses them. A program that does is refused.
- **Bus and crosspoint routing** beyond the roles above. The `$75a` mux fields stay unknown (ALGO.md §7).
- **Flag 0x100**, which adds seven extra field waits after a T-bar step. It changes only the T-bar's feel, not the frames.

## 4. Decided by me (please check)

- **Registration.** A new `HardwareGuesses::dveSourceCellOffset = 3`: a source cell is read from 4·(sc − 3). It applies to both DVE and ALGO.
  - Why: the data puts the picture 3 cells right of control cell 0 (ALGO.md §9). Without the offset, the first and last frame of every move jump 12 pixels against the clean picture.
  - Test: with the offset, T-bar 0 of a descriptor-3 move equals A pixel for pixel.
  - This changes existing DVE output. The DVE goldens are updated, and the owner judges the result by eye.
- **Wipe polarity.** Row 0 is all set (B) and row 1 is all clear (A). This is a guess from two effects (ALGO.md §6); it lives in one place so it is easy to flip.
- **Auto stepping.** During Auto, `RenderCtx::animFrame` carries the step k, as it already does for ANIMs (Ruling 25). `frameCount()` reports N for the current speed.

## 5. Tests

1. **VM goldens.** Generate JSON with `algo_ref` holding per-step H, V, cell range and line range, plus wipe spans and levels. Each program is one stateful take: T-bar 0..511, then a reversed drag, then an Auto of 30 fields. Cover Crop To Center, Squeeze T-B, Expand T-B, Slats Slip On, Mosaic Transition, Infinite Tiles, Wipe 3 Slots In, Wipe Cross Out, a border crouton, Smooth Fade, Dip To Matte, Expand Fade In and one positionable move.

   The C++ output must equal it byte for byte, which also covers the take-state model. The generator lives in `tools/ref/`, like the existing references.
2. **Whole library.** Every ALGO crouton except Art Card Key loads and renders T-bar 0, 256 and 511 without unknown-code grey. Within its line range, a descriptor-2 move at T-bar 511 equals B and a descriptor-3 move at T-bar 0 equals A.
3. **Factory.** `loadEffect` returns an effect for ALGO croutons, and the Wipes drawer in the app no longer shows ALGO croutons as unplayable.

## 6. For the owner, by eye

- Push, Squeeze and Crop moves against the effect montage in the original demo video, using frame grabs.
- The 12-pixel registration change on the DVE moves.
- Wipe polarity on a few ALGO wipes.
