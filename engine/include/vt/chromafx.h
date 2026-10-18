#pragma once
// ChromaFX: the Toaster recolours video by luma through rows of colour maps that it builds from
// an effect's stored tables. Decoded from 4.2/Programs/AuxLibs/ChromaFX (full symbols) and
// checked by running that code in a 68000 emulator; see tools/re/FINDINGS.md "ChromaFX row
// builder" and the Python reference tools/chromafx_rows_ref.py.
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "vt/crouton.h"
#include "vt/hardware_guesses.h"
#include "vt/ilbm.h"
#include "vt/raster.h"
#include "vt/timing.h"

namespace vt {

inline constexpr int kLutRecordSize = 904;
inline constexpr int kChromaEntries = 96;  // entries per table and per row
inline constexpr int kChromaRows = 480;    // DV lines the row builder writes (one per step)
inline constexpr int kChromaCycle = 0x77;  // row positions v = 0..118

// A 904-byte LUT record ('LUT\1'): the crouton's ` LUT` chunk or one slot of a bank file.
struct LutRecord {
    std::string name;     // bytes 24..39 up to the first NUL, Latin-1 -> UTF-8, whitespace trimmed
    uint8_t poster = 0;   // $4: posterize slider (editor only; already applied to the tables)
    uint8_t interp = 0;   // $5: interpolation style index (LUT_SendMap's SENDM9 table)
    uint8_t flags = 0;    // $6: bit 7 colour, bit 5 Transition, bits 2-3 cycle, bits 0-1 speed
    uint16_t tbar = 0;    // $8: T-bar position
    std::array<std::array<Rgb, kChromaEntries>, 3> tables{};  // $28, $148, $268
};

// Throws std::runtime_error for a short record or one that does not start with 'LUT\1'.
LutRecord parseLut(std::span<const uint8_t> record904);
// Bank file (ChromaFX.Effects / .Default): records from byte 32 every 904 bytes; slots that are
// not 'LUT\1' or have an empty name are skipped (as chromafx_to_cube.from_bank).
std::vector<LutRecord> readLutBank(const std::filesystem::path&);
// The ` LUT` chunk in a CRFX crouton's CrUD form; nullopt if there is none.
std::optional<LutRecord> lutFromCrouton(const Crouton&);

enum class ChromaMode { Filter, Transition };
// SENDM9 (0x848a) order: LUT_SendDVEMap, LUT_SendSpectrumDVE, LUT_SendMapRotate(d0 = 0),
// LUT_SendMapRotate(d0 = -1), LUT_SendMapNuke(d0 = 0), LUT_SendMapNuke(d0 = -1), LUT_SendPosterDVE.
enum class ChromaInterp { Spread, Spectrum, RotateRight, RotateLeft, NukeRight, NukeLeft, Poster };
// Tag 0x8000007f / $6 bits 2-3: LUT_CyCroutonTb (0x9436) -> LUT_GridCycle{Forward,Backward,Both}.
enum class ChromaCycle { None, Forward, Backward, Both };

// $5 as LUT_SendMap reads it: 0..6 in SENDM9 order, 7 and above act as 0 (Spread).
ChromaInterp chromaInterp(const LutRecord&);

// What the Switcher runs a ChromaFX crouton with. LUT_ReadNewTags (0x9916) rewrites the header
// flags from the tags when the crouton is selected, so the tags decide (all 96 shipped croutons
// agree with their headers); the interpolation style has no tag and comes from $5.
struct ChromaSettings {
    ChromaInterp interp = ChromaInterp::Spread;
    ChromaMode mode = ChromaMode::Transition;  // tag 0x80000080 != 0
    ChromaCycle cycle = ChromaCycle::None;     // tag 0x8000007f & 3
    bool colour = false;                       // tag 0x8000007e != 0 (see effect_chromafx.cpp)
    Speed speed = Speed::Medium;               // tag 0x80000055 (decodeSpeedTag)
    int tbar = 0;                              // tag 0x80000086
};
ChromaSettings chromaSettings(const Crouton&, const LutRecord&);
// The same fields from the record header alone ($6, $8; speed bits 0-1 decoded as
// LUT_ReadNewTags writes them: 0 Slow, 1 Medium, 2 Fast).
ChromaSettings chromaSettings(const LutRecord&);

struct ChromaRows {
    // 480 rows (DV lines 0..479) of 96 colours, after the Toaster's RGB -> YIQ -> quad
    // round trip. Row 479 is LUT_LinearRamp, written by LUT_CheckLinear instead of a step.
    std::vector<std::array<Rgba8, kChromaEntries>> rows;
};
// All seven styles, bit-exact with the 68000 code (quads), then dvQuadToRgb.
ChromaRows buildRows(const LutRecord&, ChromaInterp);

// LUT_TransitionMode / LUT_FilterMode: v = max(0, (tbar >> 2) - 9), tbar clamped to 0..511.
int chromaRowStart(int tbar);
// The 241-entry per-field-line table LUT_BuildTranSprite (0x9bbc) / LUT_BuildFilterSprite
// (0x9c00) send to the Toaster (sw_m318). v is taken modulo 119. Entry 0 is 0xf0.
//   Transition: entry k = 2v+2 (k even) or 2v+3 (k odd).
//   Filter: entries 1, 2, 3, ... = 2v+1, 2v+2, 2v+3, ...; after a pair ending above 238 the
//   pairs restart at 1, 2.
std::array<uint8_t, 241> chromaLineTable(ChromaMode, int v);
// The row (index into ChromaRows::rows) that raster line `line` (0..481) shows for line table
// `t`: control line k = line / 2 has value w = t[k] and the line parity picks DV line 2w or
// 2w + 1 (spec 5.10, the DVE convention). -1 when that DV line was never written (control
// line 0: w = 0xf0 -> 480, 481) or `line` is outside the raster.
int chromaTableRow(const std::array<uint8_t, 241>& t, int line);
// chromaTableRow(chromaLineTable(mode, v), line).
int rowForLine(ChromaMode, int v, int line);

// Cycle step in 16.16 rows per video field: LUT_GridCycle* index the table at 0x9b14
// {0x4000, 0x8000, 0x10000, 0x8000} with ($1f4 + 1) & 3, $1f4 = the Switcher's speed in tag 0x55
// encoding: Slow 0.25, Medium 0.5, Fast 1, Variable 0.5.
uint32_t chromaCycleStep(Speed);
// Row position v after `fields` calls of LUT_{Forward,Backward,Both}*CycleRoutine (one per
// field) from LUT_CycleModeOn's accumulator 0: forward (n * step) mod 119, backward
// (-n * step) mod 119, both: h = (n * step) mod 238, v = h < 119 ? h : 237 - h. 0 for None.
int chromaCycleRow(ChromaCycle, uint32_t step16_16, uint64_t fields);
// The row position v the Switcher uses (Ruling 20): while cycling (cycle != None) the cycle
// accumulator alone, chromaCycleRow(cycle, step, fields), and the T-bar is ignored (the
// crouton's T-bar handler is DummyTBar, LUT_ExCroutonTBarTb 0x93d6); otherwise
// chromaRowStart(tbar).
int chromaRowPosition(int tbar, ChromaCycle, uint32_t step16_16, uint64_t fields);

// out = in recoloured at row position v: per raster line the row rowForLine(mode, v, line)
// picks (black where it is -1), per pixel the entry floor(lumaOffset + Y / 4) clamped to 0..95,
// Y = Rec.601 luma (299 R + 587 G + 114 B) / 1000 of the input pixel. A non-finite lumaOffset
// counts as 0. Alpha 255.
void applyChroma(const ChromaRows&, ChromaMode, int v, const Frame& in, Frame& out,
                 float lumaOffset = HardwareGuesses{}.chromaLumaOffset);

}  // namespace vt
