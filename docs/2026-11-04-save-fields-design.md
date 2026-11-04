# SAVE "1 Field" / "4 Fields" — design

Date: 2026-11-04. Owner: "proceed with SAVE 1 Field / 4 Fields"; chose the original's composite file format over lossless RGB. Calls marked "my call" are the controller's.
Authority: `../tools/re/FREEZE.md` §3 "SAVE" (the chain through `edit.port`, the writer's flags) and `../tools/re/save_ref.py` (1 Field verified against switcher.main under emulation).

## 1. Intent

The framestore panel's SAVE button offers the original's pop-up, "1 Field" / "4 Fields", and writes the file the original writes: a 16-plane composite framestore (PLTP lanes 6/7), the format of the 75 shipped framestores, so a saved still can go back to a real Toaster. Today SAVE has no choice and writes 24-plane RGB, which the original's FRAM loader (16 planes at most) probably cannot read.

Non-goals: the original's row encoder 0x11bfd0 (its dither and phase are not traced); the 16-sample read overrun into row 478 (a composite artefact, as STABILIZE left out its own); edit's file requester look.

## 2. Behaviour

### A. The button
- SAVE opens a pop-up list "1 Field" / "4 Fields" (the STABILIZE pop-up's look and behaviour). Escape or a click outside closes it with nothing done. (proven, 0x8f392)
- Picking an item checks the existing refusals (a running transition, no still), then opens the file dialog as today. Cancel saves nothing.
- No keyboard shortcut (none traced).

### B. Unchanged
- Which still: the DV on Preview, else Main, else DV1 (the framestore design's rule). The original reads the DV that the `$75a+8` mux shows (inferred); what sets that mux is not traced, so the rule stays.
- Refusals, the save folder, the atomic write (`.tmp` then rename), the icons and CrUD, the drawer rescan, "Saved <name>".

### C. The file (proven: 0x11ba66 flags 3, both choices)
- One `FORM ILBM`, 752×480, **16 planes**, ByteRun1, aspect 10:11, PLTP `0607 0606 … 0600 0707 … 0700` (plane n → lane 6 bit 7−n, then lane 7 bit 15−n), then the icons and CrUD as today.
- File row r is raster line r + 1 (`framestoreRect`), as the loader reads it.
- **Encoding** = the exact inverse of `decodeComposite` (FREEZE.md §4): for each pixel pair x, x+1 (x even), one quad q = `rgbToDvQuad(c)`; lane 6 at px = q[px & 3], lane 7 at px = q[(px + 2) & 3]; rows with `((r + 1) >> 1) & 1` swap the lanes.
  - c = the pair's rounded mean colour (my call; the composite store has one colour per pair).
  - `rgbToDvQuad` is ChromaFX's bit-exact RGBYIQ / YIQQUAD, not the saver's own encoder (GUESS-level fidelity: the original's 0x11bfd0 dithers and is not traced).
  - Lanes 6 and 7 hold opposite-phase samples of the same quad: a still store with no motion between its colour-frame halves, which is what a vtoast still is.
- Loss (measured over all 16.7 M RGB colours): a save then load returns each pair's mean colour within **8** levels per gun for the 99.5 % of colours the composite range carries (85.6 % within 4, 99.5 % within 6; DV luma has 130 steps from black to white). The other 0.5 %, over-saturated colours, are clamped by up to 32, as composite on the Toaster clamps them. The owner chose the original's format over lossless RGB.

### D. 4 Fields (proven: 0x844ec saves the whole store)
- The still as it is, encoded as C.

### E. 1 Field (verified: `save_ref.py`, model == switcher.main)
- File rows 1, 3 … 477 = the still's raster lines 2, 4 … 478 (DV lines 2 … 478: the even field, the one STABILIZE 1 Field drops).
- Rows 2, 4 … 476 = (row above + row below) >> 1 per R, G, B (floor).
- Rows 0, 478 and 479 are black.
- Then encoded as C. The still in DV1/DV2 is not changed.
- The crouton icon is drawn from the picture as saved (my call; the original's icon routine 0x112bca is not traced).

## 3. Shape
- **Engine**: `enum class SaveFields { One, Four }`; `saveFramestore(path, still, SaveFields, whyNot)` writes 16-plane composite; the 1 Field line law is applied to the picture before encoding, inside `framestore.cpp`.
- **App**: `SwitcherActions::saveFramestore(vt::SaveFields)`; the panel's SAVE opens a `ListPopup` {"1 Field", "4 Fields"}; the controller's refusal check, dialog and posted save take the choice.
- **Tests**: engine — the saved file's BMHD and PLTP; save then load within the measured tolerance on a colour sweep and on `Framestore/ColorBars`; 1 Field line law (kept rows, averaged rows, black rows 0 / 478 / 479); app — the pop-up opens on SAVE, each item reaches the save with its choice; e2e save of each choice loads back.
