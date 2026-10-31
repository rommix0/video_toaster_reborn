# STABILIZE on the Switcher — design

Date: 2026-10-31. Owner: "switcher leftovers first, starting with STABILIZE". Calls marked "my call" are the controller's.
Authority: `../tools/re/FREEZE.md` §3 "STABILIZE" (the trace) and `../tools/re/stabilize_ref.py` (both choices verified against switcher.main under emulation).

## 1. Intent

The framestore panel's STABILIZE button ("Motion Removal" in the code) stops a frozen still from jittering. Today it is a stub ("not in version 1"). This makes it work as the original does, as far as a decoded progressive still can show it.

Non-goals: the composite-level work that has no visible counterpart in a decoded frame (colour-phase rebuild, the 4-field colour frame, the decode/re-encode dither, line-end artefacts, DV line 0 at half luma); the capture's duration; SAVE "1 Field" / "4 Fields".

## 2. Behaviour

### A. The button
- STABILIZE opens a pop-up list "1 Field" / "2 Fields" (the BKG pop-up's look; opens above the button). The button shows pressed while the list is open. Escape or a click outside closes it with nothing done.
- **Alt+f** = "2 Fields" (proven, 0x96322).

### B. What both choices do first (FREEZE.md §3, proven)
- An engaged ChromaFX disengages (as a framestore load does).
- A live store freezes, as FREEZE does (the live picture goes into a still by the FREEZE rule).
- No bus change. The CG key page, if any, stays loaded.
- **The DV worked on**: DV1 if Main shows DV1, otherwise DV2 (proven; Main on an input or BKG gives DV2). The other DV is untouched.
- Refusals (my call, the shared-DV rule of the framestore design): refused with "finish the effect first" while a transition runs and nothing is engaged (as loadStill), and while a grab is waiting (the original's grab blocks, so a stabilize after it would apply to the grabbed picture). The original refuses nothing else.

### C. 1 Field (verified; raster y = DV line, kFramestoreY0 = 1 matches the FRAM loader)
- Raster lines y = 1, 3, …, 479 are kept.
- Each line y = 2, 4, …, 478 becomes `(line y−1 + line y+1) >> 1` per R, G, B (floor; alpha stays 255).
- Line 480 becomes black. Lines 0 and 481 are unchanged.
- Visible result: combing from interlaced motion is gone; vertical detail halves.

### D. 2 Fields (verified)
- **The picture does not change.** The original's motion test compares the two halves of the 4-field colour frame; a vtoast still is one frame (2 fields), so nothing counts as moving. The manual's own reading agrees: "2 fields (frame)" — a vtoast still already is one.
- The steps in B still happen (freeze, ChromaFX off).
- Status bar (vtoast's addition): "STABILIZE 2 Fields: the still is already one frame".

### E. Timing
- Instant (my call). The original takes seconds (a capture per DV line, guess); there is nothing to capture here.

## 3. Shape
- **Engine**: `Switcher::stabilize(StabilizeFields, std::string* whyNot)` with `enum class StabilizeFields { One, Two }`, beside `setFrozen` / `loadStill`. 1 Field works on the still in place.
- **App**: `SwitcherActions::stabilize(vt::StabilizeFields)` posted with `postChecked`; the BKG pop-up becomes a general list pop-up (item index callback) used by both; `fs_stabilize` leaves the stubs (none remain).
- **Tests**: engine — target DV choice, freeze-if-live, ChromaFX off, refusals, 1 Field line law on a picture with distinct lines (incl. lines 0, 480, 481), 2 Fields unchanged; app — pop-up opens, each item posts its choice, Alt+f.

## 4. Follow-ups noted
- SAVE "1 Field" / "4 Fields" (FREEZE.md §6, still open).
