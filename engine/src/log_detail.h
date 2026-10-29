#pragma once
// The engine's message helpers (vt/log.h); not part of the public API.
#include <filesystem>
#include <string>
#include <string_view>

namespace vt::detail {

// A path as UTF-8 for messages: path::string() throws on MSVC for names outside the ANSI code
// page ("Bärs ✓").
inline std::string utf8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}

// Hands one warning line to the sink set by setLogSink (default: stderr).
void warn(std::string_view message);

}  // namespace vt::detail
