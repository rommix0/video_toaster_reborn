# vtoast_reborn — Switcher design (version 1)

Date: 2026-10-17. Status: draft for approval.

## 1. Intent

vtoast_reborn is a modern desktop recreation of the NewTek Video Toaster (4000, software
4.x), starting with its **Switcher**. The owner wants the Toaster's effects to behave
exactly as they did on the original hardware: frame-for-frame timing, original
colours, and **DVE moves that are bit-exact**. An OBS plugin was considered and
rejected because a plugin cannot reproduce the Toaster's pipeline faithfully
(pre-rendered stingers cannot move live video, and OBS works at HD with its own scaling).

There is no Toaster card or Amiga any more. vtoast_reborn replaces the hardware in
software: the Amiga drew control pictures, and the Toaster hardware used them to mix
and move video. We do both steps in code.

The program will grow to include ToasterPaint, LightWave 3D (Modeler + renderer) and
the Flyer editor as tabs in the same window. Each of those gets its own design later.
This document covers the shared foundation and the Switcher only.

## 2. Decisions made by the owner

* One desktop application; modules are tabs (Switcher first; Paint, LightWave, Flyer later).
* **Qt** for the interface; **C/C++**, **CMake**; **FFmpeg** libraries for capture,
  decoding, encoding and recording. **No libobs** and no OBS code copied. OBS
  (`C:\Users\abart\Desktop\obs-studio-master`) is a reference for how a modern switcher
  app is organised, and vtoast_reborn may resemble it in places (settings dialog,
  docks, output settings) without copying it.
* **Fixed Toaster sources**, as on the original: Inputs 1–4, DV1, DV2, BKG (background
  colour generator). Each input is mapped by the user to a capture device, screen/window
  capture, or a video file.
* **Original internal resolution, progressive**: the engine works at the Toaster's own
  NTSC picture size (4:3, 480 lines) but **progressively, at 59.94 frames per second**;
  inputs and outputs are progressive. No rescaling of effects; output is shown at native
  size or whole-number zoom. Rescaling options may come later as an extra. (How the
  field-based effect data maps onto progressive frames: see 5.10.)
* Effects follow each effect's own speed settings (most default to 29.97 fps; see 5.3).
* **Clean video**: no simulation of NTSC/composite artefacts or of the analog signal
  path (that would be a separate project). Effects themselves are reproduced faithfully.
* **Audio passes through**: input audio goes to Program and recordings, mixed properly
  when switching between sources, together with the effects' own sounds.
* **Interface:** layouts follow the original Toaster screens as closely as Qt allows;
  **original image assets (buttons, croutons, fonts, panels) are reproduced pixel for
  pixel**, displayed at whole-number scales.
* The original Toaster data (effects, images, fonts) is included with the build.
  Licensing is deliberately left until the end of the project.
* Platform: Windows first (the machine has Visual Studio 2022 and CMake 3.31).

## 3. Version 1 scope

Must work in version 1:

1. **Video and audio in and out:** four live inputs (FFmpeg: DirectShow devices,
   screen/window capture, video files) with their audio, Program output in a window,
   and recording Program (progressive 480p, 59.94 fps, with mixed audio) to a file.
2. **Switcher core:** Main (Program), Preview and Overlay bus rows, each with
   001–004, DV1, DV2, BKG buttons; T-bar; Auto; Take; cut and dissolve.
3. **Effects from the original crouton files:** gradient (ILBM) wipes, animated (ANIM)
   wipes with their palette/background colours and overlays, effect sounds, and effect
   speeds (Slow/Medium/Fast).
4. **DVE moves, bit-exact** (the "hardware ramp" ANIMs).
5. **ChromaFX:** Filter, Transition and Cycling modes, from the original ChromaFX
   croutons and banks.
   *Builder's call:* in version 1 the Overlay row is present and selectable, and
   overlay-type effects use it where the original did; it has no keying until keys
   arrive. DV1/DV2 are real framebuffers used as the working memory of DVE moves and
   ChromaFX; selected on a bus they show their contents (black until freeze/framestores
   exist).
6. **A basic original-look Switcher screen:** effects bank grid with the original
   crouton icons, bus buttons and T-bar using extracted assets where available; plain
   Qt controls may stand in where an asset is not extracted yet.

Later, one at a time, in roughly this order: Switcher moves (ALGO croutons, needs
reverse-engineering in Ghidra), DV1/DV2 freeze and framestore load/save, keys (luminance
key / Overlay), the full original interface, streaming and virtual camera. Then
ToasterPaint, LightWave, Flyer (separate designs).

## 4. Non-goals and anti-goals

Non-goals for version 1: ALGO Switcher moves (they may appear as clearly labelled
placeholders, or be absent), CG, LightWave, Paint, Flyer, streaming, virtual camera,
HD/upscaled internal processing, interlaced output, NTSC/analog signal simulation
(including the DV path's 1/15 s delay and stutter), macOS/Linux builds.

Anti-goals (would make it a failure even if it works):

* Effects that look "close" but not like the Toaster: smoothed/blurred effect edges,
  resampled DVE pixels, wrong timing, colours that drift from the originals.
* Smearing or invented picture at the edges (the converter's overscan fill must not be
  repeated; see 5.6).
* Pre-rendered effect videos inside the engine. The engine must read the original
  crouton files and compute every frame.
* A generic OBS clone with Toaster paint on top.

## 5. What the Toaster actually does (facts the builder relies on)

All of this was reverse-engineered in this workspace. Reference implementations exist in
Python and are the **ground truth for tests**:

* `tools/toaster_iff.py` — IFF/ILBM/ANIM/8SVX/CrUD decoding, NewTek op-7 and op-8.
* `tools/toaster_fx_convert.py` — how palette indices become mattes/overlays.
* `tools/toaster_dve.py` — DVE control decoding.
* `tools/chromafx_to_cube.py` — ChromaFX LUT record parsing.
* `tools/re/FINDINGS.md` — how the Switcher and ChromaFX work (from disassembly);
  `tools/re/` also holds the hunk loader, disassembler and Ghidra setup script. The
  Ghidra project `vtoast_ghidra/vtoast` has both programs labelled.

### 5.1 Croutons (effect files)

IFF `FORM CrUD` with `TYPE` (ALGO, ILBM, ANIM, CRFX, MAIN, CTRL, …), `LIBS` and `TAGS`,
followed by the effect's own FORMs (ILBM gradient, ANIM animation, 8SVX sound). Each
crouton has a sibling `.i` file: its 80×50 icon (ILBM, 6 planes) for the effects bank.
3.x effects can be Amiga hunk files wrapping the same FORMs (`unwrap_hunk`).

Tags (simple tags `0x8000xxxx` value; `0x1000xxxx` carry a byte blob). Defaults per
type live in `crouton.library` (`_ANIMtaglist`, `_ILBMtaglist`, …); a missing tag means
the default.

| tag | meaning |
|---|---|
| 0x80000022 | effect mode: 4/5 soft matte, 7 colour roles, 0/2 DVE |
| 0x80000023 | background colour 1 Black 2 White 3 Red 4 Green 5 Blue 6 Yellow 7 Magenta 8 Cyan 9 Snow; negative → use 0x80000024 |
| 0x80000024 | custom background colour (12-bit) |
| 0x80000055 | default speed button (ANIM default 1 = Medium) |
| 0x80000056–59 | Variable/Slow/Medium/Fast for 68020+ machines |
| 0x8000005a–5d | the same for 68000 machines (vtoast_reborn uses the 68020 set) |
| CRFX 0x80000080 | 1 Transition mode, 0 Filter mode |
| CRFX 0x8000007f | cycle: 0 none, 1/2/3 directions |
| CRFX 0x8000007e | colour (1) or black-and-white (0) |
| CRFX 0x80000086 | stored T-bar position (0–511) |

### 5.2 Animation decoding

ANIM frames are full ILBM then deltas: op-5 (standard), NewTek op-7 (per-plane streams,
16-bit pointer wrap, count −1 = jump), NewTek op-8 (12-byte per-plane records, filled
backwards). **Reference frame rule:** frame *n* is a delta from frame *n − interleave*;
while fewer than *interleave* frames exist, from frame *n − 1*. (Verified: 20 OFF !
and 20Off.aa decode identically only with this rule.) Writes past the bitmap are dropped.

### 5.3 Effect timing

Every ANIM delta has reltime 1; the speed comes from tags. For ANIM croutons the
Slow/Medium/Fast tags are **fields per animation frame** (defaults 3/2/1; 0 = that speed
button disabled). Duration in video frames = (fields_per_frame × frame_count + 1) / 2
(`switcher.main` 0xe6548). So Fast = 59.94 fps, Medium = 29.97 fps (the usual default),
Slow = 19.98 fps. For ILBM/ALGO croutons the same tags are durations in frames
(defaults 90/60/30). The T-bar range is 0–511.

### 5.4 Animated and gradient wipes

* Gradient (ILBM) wipe: the Toaster reveals bright areas first; the T-bar threshold
  sweeps the gradient. Soft edge as in the original (to be confirmed in code; the
  converter's softness is not authoritative).
* ANIM wipe, by mode: soft (index/max of up to 4 planes is the mix), roles (dominant
  first-frame index = A, dominant last-frame index = B, others = colour from the frame's
  palette, or the background colour when the palette is black), overlay-only effects,
  flash effects. `toaster_fx_convert.py` documents these rules; the builder should confirm
  them against `switcher.main` where they matter.
* Effect sounds: 8SVX, start at the first frame.

### 5.5 DVE moves

Control frames of 768 pixels × 241 lines (one field). Per 4-pixel cell an 8-bit value
(plane 1 nibble high, plane 0 low): 0 background; 64–255 → source column cell 255 − v;
1–15 shadow; 16–23 colour-bar fills. Plane 2 rows 0–10 hold the per-line source-line
table (after a 2-word header, high nibbles of word pairs). Planes 3+ (without the
152D0683 signature) are an artwork layer, colour = CMAP[group × 8]. Which clip moves and
"flip" behaviour: see `toaster_dve.py` / `toaster_fx_convert.write_dve`.

**Bit-exact** means: for every output pixel of every field, the engine reads exactly
the source pixel the Toaster would have read (cell/column and line from the control
data), with no filtering, and applies shadows/fills as the hardware did. The exact
shadow strength and the registration between the 768-wide control grid and the DV
picture (752×480, see 5.7) must be established from the code/data, not guessed.

### 5.6 Overscan

DVE control data has stale, never-cleared margins (about 13 lines top, 12 bottom,
48 pixels left, 80 right). On a TV these were hidden by overscan. The converter filled
them by copying the nearest row/column, which produced visible smearing: **do not do
that.** Keep the raw data; display the visible picture area by default, with an
"underscan" toggle that shows the full raster. Because the clean area is narrower than
a normal TV crop (~640 of 768 pixels), find out whether the hardware blanked part of it
(check `switcher.main` and the DV picture size) before choosing the default crop.

### 5.7 Framestores and the internal picture

Framestore files (`4.2/Framestore`, `Programs/Switcher_Support/StartupFrames`) are ILBM
752×480, 16 planes, with a Toaster `PLTP` chunk; this is the DV framebuffer's native
picture. The builder must decide the exact internal raster (752×480 progressive, as
the DV picture, vs the effects' 768-wide grids) from this data and the DVE registration.
Framestore loading itself is after version 1, but the raster decision is not.

### 5.8 ChromaFX

* 904-byte `LUT` records (crouton ` LUT` chunk, or 100 per bank file
  `ChromaFX.Effects`/`.Default`): 40-byte header (name at 24), three 96-entry RGB tables
  indexed by luma (entry = 17.5 + Y·255/4).
* The Toaster interpolates table 0 → table 1 into rows (`LUT_SendDVEMap`: 480 steps,
  RGB → YIQ via `RGBYIQ`/`YIQQUAD`; interpolation style spread/spectrum/rotate/nuke/
  poster, `$5(a4)`), stores them in DV memory, and each field picks a row per line:
  * Transition mode (`LUT_BuildTranSprite`): every line uses row = f(T-bar).
  * Filter mode (`LUT_BuildFilterSprite`): consecutive lines use consecutive rows
    (graduated filter that slides with the T-bar).
  * Cycling: the row advances every field at the stored speed.
* It recolours the source on the Preview bus (fallback DV2) and outputs through DV2;
  it never switches A→B itself. It shares DV memory with DVE moves, so the two cannot
  run at once — reproduce that restriction.
* `tools/chromafx_to_cube.py` handles parsing and table picking but its .cube output is
  an approximation (luma-only, phase-picked); the engine must reproduce the row
  interpolation and YIQ quantisation from the disassembly instead.

### 5.10 Fields to progressive frames

The Toaster's effect data is per field: a 241-line control frame drives one field
(every other picture line). vtoast_reborn runs progressive 59.94 fps frames of 480 lines:

* Each output frame uses one effect control frame for both line parities: control line
  *k* drives picture lines 2*k* and 2*k*+1, and the DVE reads source lines in the
  matching parity (2·src and 2·src+1).
* At Medium speed (two fields per effect frame, the usual default) this equals the
  Toaster's two woven fields exactly. At Fast speed the Toaster's two fields came from
  consecutive effect frames; here each effect frame becomes one whole progressive frame.
  This is the only intended departure from the original, and it involves no blending
  or resampling.
* Effect timing (5.3) is counted in 59.94 fps frames instead of fields; the numbers
  are unchanged.

### 5.11 Audio

The Toaster switched no audio; vtoast_reborn adds it (owner's decision). *Builder's
design, owner may override:* audio follows video. Each input's audio is captured with
its video; during a transition the outgoing and incoming sources' audio crossfade along
the transition's progress (T-bar position), with an equal-power curve; a cut is a short
de-clicked switch. Effect sounds (8SVX) are mixed on top, starting at the effect's first
frame. An OBS-style audio mixer gives each input (and effect sounds) a level fader,
mute and meter, plus a master level. Audio and video stay in sync (shared clock;
FFmpeg timestamps).

### 5.9 Background colours

BKG and effect colours come from the background colour generator (5.1 tag 0x23):
eight solid colours plus Snow (noise). Exact YIQ/RGB values must come from
`switcher.main` (the generator's table), not from guesses.

## 6. Shape of the program (builder's design, with these constraints)

My calls, which the owner can override:

* **Engine library** (C++17 or later, no Qt dependency) holding: crouton/IFF decoding,
  the effect players, the DVE and ChromaFX processors, the field clock, and the bus
  mixer. It must be testable without a GUI.
* **Video I/O** module on FFmpeg (`libavdevice` for DirectShow/gdigrab capture,
  `libavformat`/`libavcodec` for files and recording, `libswscale` only on the way in).
  Inputs are converted once to the internal raster and field timing; nothing is scaled
  after that.
* **Qt application** with a tab bar (Switcher; Paint, LightWave, Flyer as disabled
  placeholder tabs), Program/Preview monitors drawn at native size or integer zoom, and
  an OBS-style settings dialog for inputs and recording.
* **Asset extraction** tooling (offline, run at build time or once) that turns the
  original files into the images/fonts the UI uses: crouton `.i` icons, Amiga colour
  fonts (`System_Support/Fonts`: InterfaceDG/LG/TABS, NewToaster, TimeCode, CGInterface),
  and interface artwork embedded in `switcher.main` / `AuxLibs/ChromaFX` (the ChromaFX
  library's symbols name its images, e.g. `LUT_TBar_Img_PTRS`). Extracted assets keep
  their original pixels; scaling is by whole numbers with nearest-neighbour.
* Folder: `vtoast_reborn/` (this `docs/` folder plus source). The original Toaster
  data stays where it is and is copied/packaged into the build.

## 7. Verification

* Decoder tests: the C++ decoders must produce byte-identical frames to
  `tools/toaster_iff.py` for every crouton in `4.2/Effects` and the 3.1 folders
  (manifest: `converted/manifest.csv`).
* DVE tests: control decoding must match `tools/toaster_dve.py` exactly (ignoring its
  overscan fill), and rendered fields must match a reference built from the same
  mapping with integer source lookups.
* Timing tests: duration and per-frame field counts match 5.3 for every speed.
* ChromaFX tests: interpolated rows match a reference built from the disassembled
  routines.
* **What "bit-exact" is checked against.** No Toaster hardware is available. It means
  exactly faithful to the decoded effect data (cell/line tables, palettes, timing).
  Where the answer lived in the card's circuits rather than the code (which pixel in a
  4-pixel cell is read, shadow strength, grid-to-picture registration, wipe soft edge,
  ChromaFX colour handling), the builder picks the best-supported value, documents it
  as a hardware guess, and keeps it as one adjustable setting. When the Python reference
  and the disassembly disagree, the disassembly wins.
* Visual reference: `Video Toaster 4000 Demo.mkv` in the workspace root (an 8-minute
  VHS rip of the 90s demo reel, 640×480 29.97p, already deinterlaced). Good for
  comparing looks, colours and timing by eye; not for pixel comparison.

## 8. Left to the builder

Exact internal raster and DVE registration (5.5, 5.7); default overscan crop (5.6);
gradient wipe edge softness, shadow strength and background colour values (from code);
threading and frame pacing; audio device handling for effect sounds; recording formats;
how FFmpeg and Qt are obtained (none of them are installed as development libraries on
this machine yet: Qt is not installed, and only the FFmpeg command-line tool is present).
