# Keys and the Superimpose bus: design

Status: draft for the owner's review (2026-10-29).
Reverse-engineering source: `../tools/re/KEY.md` (keyer, Superimpose, Take/Auto, Art Card Key), with `key_scan.py` for the per-descriptor table; plus `FREEZE.md` §0 (bus variables) and `ALGO.md` §7, §15 (routing descriptors, the T-bar walker). User-facing behaviour: `manual_notes.md` §1 and §5.

## 1. Intent

The Superimpose row and the key panel are on the screen today, but nothing keys. Version 1 left keying out (switcher design §3: the Overlay row "has no keying until keys arrive"). The four key controls (OFF, black key, white key, level box) are stubs that say "not in version 1".

The owner wants the Toaster's keyer as the original had it. The Toaster only does **luminance keying**: a Superimpose source is laid over Main, and its own brightness, against a clip level, cuts the hole (manual §5). The original also lets the digital moves fly a key on and off, and has one special crouton, **Art Card Key**, that turns the T-bar into the clip control.

## 2. Goals

### A. The keyer and the Superimpose bus (the panel)

State, all in `Switcher` (KEY.md §1, proven):
- **Key mode**: off, Key Black or Key White. Off keeps the last polarity. Power-up: off, White.
- **Clip level** L, 0–257. Power-up: 128.
- **Superimpose source**: none, inputs 1–4, DV1, BKG, or live digital (as on Main). The bus can be empty, unlike Main and Preview.

What the controls do (KEY.md §1, §3, proven):
- **Picking a Superimpose source** (buttons, q–u) turns the keyer **on**, keeping its polarity. Pressing the lit source again does nothing.
- **DV2 alone on Superimpose is refused**: the bus keeps its previous source. DV1 and live digital are accepted. Live digital on Superimpose follows the framestore design's rule unchanged (one DV input; every bus showing live DV shows the same delayed input), so DV2 is refused only when the result would be the plain DV2 still (store frozen, or nothing on the bus to make live). Builder's call: the original's "only one live-digital bus" stripping is not added.
- **Black key / White key** (buttons, `[` / `]`) turn the keyer on with that polarity. With an empty Superimpose bus the button lights but nothing is keyed.
- **OFF** (button, `o`) turns the keyer off **and empties the Superimpose bus**.
- **Level box**: dragging with the left button held changes L by half the mouse movement, clamped 0–257, shown as three digits at the original's place (SCREEN.md: IntuiText at (656,413)).

The picture (KEY.md §2): a key shows when the keyer is on, Superimpose is not empty, Superimpose differs from Main, and no effect owns the mixer (no transition running; B says what each effect does to the key).
- L = 0: the **Superimpose source alone**.
- L = 257: **Main alone**.
- L = 1–256: the clip value **v = (Black ? L : 257 − L) − 1** (0–255, exact, proven), and the key law in §4.

### B. Take, Auto and the T-bar with a key up

**Take** (KEY.md §4, proven), in this order:
1. Main and Preview swap.
2. If the old Preview equalled the old Main and Superimpose was not empty, Preview becomes the Superimpose source.
3. Superimpose is emptied (so no key shows).

The key mode is **not** changed: the Black/White button stays lit with an empty bus.

**Auto and the T-bar** have no key dissolve of their own. They run the loaded effect, and the effect's **routing descriptor** (tag 0x80000007; ALGO.md §7) decides what happens to the key at take start. Two families (`key_scan.py`, proven):
- **Drops the key** (descriptor flag 0x4: descriptors 1, 4, 8, 11, 12, 20, 22–27, 29–32 and the inline 0x405 ones; every dissolve, wipe, overlay, Dip To Matte, the "!" DVEs with descriptor 32, Art Card Key's base): the keyer turns **off** at take start, and the effect runs as it does today. The Superimpose bus is emptied at the end of the take.
- **Flies the key** (keyer on at take start, descriptors 2, 3, 5, 6, 17, 18, 19: all 310 digital moves and DVEs without flag 0x4): the move keys its moving picture over Main instead of switching Main and Preview.
  - **Fly on** (2, 17; B moves): the moving picture is Preview's. When the move ends, **Preview is the key**: Superimpose holds Preview's source, the keyer stays on, and Main is unchanged.
  - **Fly off** (3, 18; A moves): the moving picture is the key source. When the move ends, the **key source goes to Preview**, Superimpose is emptied, the keyer stays on, and Main is unchanged. This is the manual's "the keyer stays active, the key source sits on Preview ready to fly back on".
  - **Two-phase moves 5, 6 and 19** (34 croutons; inferred from `algo_desc.py`: their keyer-on steps are `00 01 02` before T-bar 256, `02 01 00` after, final `00 01 02`): the first half keys the Superimpose source over Main; at T-bar 256 Main and the key source swap roles (the second half keys Main's picture over the Superimpose source); at the end the buses are as at take start and the keyer stays on. 6 is in this group, not with fly on.
  - **Trigger**: keyer on at take start and something to fly. Fly off with an empty Superimpose bus has nothing to fly, so it runs as a normal (unkeyed) move. Fly on needs no Superimpose source (Black/White with an empty bus is the usual way in); with Preview == Main the key shows nothing (the key condition fails) and the end state still puts Preview's source on Superimpose, as the original does. Builder's call for the fly-off guard.
  - During the move, the moving picture is keyed by its own luma against L, over Main. ALGO fade-level moves (Expand Fade In, Squeeze Fade Out; ALGO.md §15.4) **ignore their mix level in key mode**: the mixer is in key mode (3), where the A/B mix level does not apply. A GUESS (§6). Around it, ALGO moves fill with Black or White (code 0xe8/0xe9, the polarity's colour) so the area is keyed away (KEY.md §4, proven code, inferred meaning). DVE ANIMs get the same: key-0 area = the polarity's colour (a GUESS, §6).
- **Effects with no descriptor tag** behave as descriptor 1 (drops the key). Builder's call.
- **Engaged ChromaFX and overlays**: no key shows while any effect is engaged or a key-dropping transition runs. ChromaFX recolouring the Superimpose source (KEY.md §2, `$2a0` = 4) is a non-goal. Builder's call.

**Changes during a take** (KEY.md §4, proven): with descriptor flag 0x1, a key-mode change is reverted; with flag 0x400, a level change is reverted. Otherwise the change applies at once.

**Back to T-bar 0** (by hand or reverse Auto): a key-dropping effect leaves the keyer off (it is not turned back on); a key-flying move puts the key source back on Preview and empties Superimpose (KEY.md §4, proven).

### C. Art Card Key

Croutons `Effects/{2000FX,4000FX}/Control/ART CARD KEY` (ALGO kind 5; refused today). KEY.md §6 (proven code, inferred meaning):
- Loading it turns the keyer on. The T-bar **is the clip control**: T-bar 0–511 sets L = T-bar / 2 (0–255), live, both ways. The link runs both ways: changing the level box sets the T-bar to 2L, and L is clamped to 255 while the card is loaded (KEY.md §6 step 4).
- In the engine the card is **held**, like an engaged ChromaFX: the T-bar never starts a transition, 511 does not swap, Auto and reverse Auto do nothing, and FREEZE/LIVE and framestore loads are refused while the T-bar is above 0 (the existing transitionActive rule).
- The picture: **Main is keyed over Preview** through the Superimpose source's luma (the "art card" cuts the hole), with v = Black ? L : 255 − L (no −1, unlike A).
- **Take** cuts Main and Preview but **keeps the card**: an emptied Superimpose bus is restored from the source the card saved (KEY.md §6 step 3). Black/White re-apply the card with that polarity; OFF ends it (keyer off, as KEY.md §6 step 2). Loading another effect ends it. Inferred from the handler; builder's call where KEY.md's step order is ambiguous.

### D. Snapshot, UI and keyboard

- `SwitcherSnapshot` gains key mode and clip level, so the panel lights OFF/Black/White and prints the level, as it lights FREEZE/LIVE today.
- The four stubs become real controls. The `o`, `[`, `]` keys work as on the original (always: vtoast always has an effect loaded).
- Refusals (DV2 alone) go to the status bar like the other bus refusals.

## 3. Non-goals

- **CG key pages** (KEYF, alpha modes 4/5): there is no ToasterCG yet. They come with CG.
- **The analog-Superimpose-through-DV routing** (KEY.md §2, 0xaf326: the store freezes and a LUT ramp is written). Its purpose is unknown, and it changes the FREEZE state as a side effect. The key fill comes straight from the source. Builder's call; revisit if the owner sees the original freeze when a Superimpose input is picked.
- The second clip pair (`$3c`), the glitch guard, ARexx, the hidden key hotspot, the Bar croutons' clip writes (the Bars are refused anyway).
- The right-edge artefact the manual mentions on digital keys.

## 4. The key law (hardware, a GUESS)

The program shows only v and the polarity; how the Toaster turns them into a picture is not in the code (KEY.md §8.1). The design, in one place (`HardwareGuesses`), easy to change:
- **Key signal**: the Superimpose source's luma, Y = (77 R + 150 G + 29 B + 128) >> 8 (Rec. 601 weights, 8-bit).
- **Key White**: Superimpose shows where Y ≤ v, Main where Y > v. **Key Black**: Superimpose shows where Y ≥ v, Main where Y < v. This fits the manual: White level 1 (v = 255) keys out only the lightest parts; Black runs the other way.
- **Softness**: a linear ramp `keySoftness` levels wide across v. Default 0 (a hard key, as the manual says an analog key is sharp).
- Art Card Key uses the same law with Main as the fill and Preview as the background.

## 5. Builder's calls

- `Bus::Overlay` is renamed `Bus::Superimpose` (13 uses), to stop it being confused with the Overlays-drawer effects (`isOverlay()`).
- Effects expose their key behaviour as one small enum (drops / flies on / flies off / keeps across the swap), filled from the routing descriptor by the effect factory, and Art Card Key as its own effect type.
- The loop-thread rule stays: every key control is a posted, checked command, as FREEZE/LIVE is.

## 6. To judge by eye (GUESSES)

1. The key law (§4): hard threshold, and Rec. 601 luma.
2. A dissolve (Smooth Fade) with a key up: in this design the key **drops at take start**, then Main dissolves to Preview. The code shows the keyer bit cleared at take start, but a kind-2 fade leaves the mixer in key mode, so the original may fade the key off with the picture instead (KEY.md §8.3). If the owner remembers "Auto dissolves the key off" visibly, switch this one case to "the outgoing picture is Main with its key".
3. DVE ANIMs in key mode: the key-0 area is the polarity's colour so it keys away.
4. Art Card Key's reading (fill = Main, background = Preview).
5. Keyed fade-level moves ignore their mix level; keyed two-phase moves (5, 6, 19) swap key and Main at T-bar 256 and end where they started.

## 7. Acceptance

- Picking In2 on Superimpose with In1 on Main at L = 128, Key Black: pixels of In2 darker than v show In1, the rest show In2. L = 0 shows In2 alone, L = 257 In1 alone.
- OFF empties Superimpose. DV2 alone is refused. Take with Preview == Main moves the key source to Preview.
- Smooth Fade with a key up: the key is gone from the first field (until §6.2 is decided). Push On L-R with a key up and Preview = In3: In3 flies on as a key, and at the end Superimpose = In3, the keyer is on, Main is unchanged. A fly-off move with the keyer on and an empty Superimpose bus runs as a normal move.
- Art Card Key: the T-bar moves L between 0 and 255 and back, live; with Key Black, T-bar 0 gives v = 0 and 511 gives v = 255, and the picture is Main keyed over Preview.
- The panel lights and the level text follow the engine's state; `o`, `[`, `]` work.
