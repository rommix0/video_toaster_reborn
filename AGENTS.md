# AGENTS.md

Notes for AI coding agents working in this repository. See `README.md` for what the project is.

## Build and test

- `tools\build.bat` configures, builds (Release, Ninja, MSVC x64) and runs every test. From Git Bash: `cmd //c "tools\\build.bat"`. Extra arguments go to `ctest`, e.g. `tools\build.bat -R vt_tests`.
- `tools\dist.bat` runs `tools\build.bat`, then packages the app as `build\dist\vtoast-reborn-alpha-<commit>\` and `.zip`.
- `tools\live.bat` runs only the tests that need devices or a desktop (CTest label `live`).
- A change is done when the full `tools\build.bat` passes (all suites green).
- The tests read the original Toaster data from `data/` (`VT_DATA_ROOT`), which `tools\extract_disc.bat "<v4.3 CD image>"` fills. It is git-ignored and must not be added.

## Where things are

- `engine/`: the Switcher model and effect renderers, no Qt. `media/`: inputs, the program loop, audio, recording (FFmpeg). `app/`: the Qt application. `assets/`: panel and crouton art read from the original files. `cli/`: `vt_render`, `vt_show`.
- `docs/YYYY-MM-DD-<topic>-design.md`: one design per feature; `docs/plans/`: implementation plans.
- Reverse-engineering notes and Python reference models live **outside** the repo in `../tools/re` (e.g. `FREEZE.md`, `CGSCROLL.md`, `*_ref.py`). Designs cite them as the authority.

## How features are made

1. Trace the behaviour in `switcher.main` (notes and a `*_ref.py` model in `../tools/re`).
2. Write the design in `docs/`, marking each behaviour proven / inferred / guess.
3. Tests first, then code. Where a reference model exists, a script in `tools/make_*_golden.py` writes `tests/golden/*.jsonl` (CRC32s) and the C++ must match exactly.
4. Review, fix, full build green, then the owner tries it in the app.

## Rules

- **Branches**: every fix or feature on its own branch. Merge to `main` (fast-forward) only when the owner says "Merge to main". Don't push.
- **Commits**: plain messages, no attribution or co-author lines.
- **Faithfulness**: behaviour follows the original. Where the original doesn't say how the hardware behaves, put the choice in `HardwareGuesses` (`engine/include/vt/hardware_guesses.h`) with a `HARDWARE GUESS` comment citing the RE note, so it can be tuned by eye.
- **Threads**: every Switcher change from the UI goes through `ProgramLoop::post` / `postChecked`; the UI reads the snapshot. `Switcher::render` stays allocation-free in steady state and never throws.
- **Line endings**: keep each file's own (most LF, a few CRLF); never rewrite a whole file's endings.
- **Tests**: drive widgets with QTest events only. Never move the real desktop pointer, inject OS input, or record the microphone.
- **Downloads** need the owner's explicit permission.
- Leave the untracked `session_memory.json` alone; it is not part of the project.
