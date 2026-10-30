#pragma once
// Control croutons (Effects/*/Control): not effects but Switcher moves (FREEZE.md 4, handler 0x120fa4).
#include <filesystem>
#include <optional>

#include "vt/background.h"
#include "vt/switcher.h"

namespace vt {

// A MAIN crouton (Video Source 1-4, Black ... Snow) puts its tag 0x80000076 bus mask on Preview,
// after setting the BKG colour to its tag 0x80000023 preset (the colour ones, with BKG). A CTRL
// crouton (Blank, Stop) drives the project sequencer: both fields empty.
struct ControlMove {
    std::optional<Source> preview;
    std::optional<BkgColour> background;
};

// nullopt when the file is not a MAIN or CTRL crouton.
std::optional<ControlMove> loadControl(const std::filesystem::path& path);

}  // namespace vt
