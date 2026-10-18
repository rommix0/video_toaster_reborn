#pragma once
// The engine's warning helper (vt/log.h); not part of the public API.
#include <string_view>

namespace vt::detail {

// Hands one warning line to the sink set by setLogSink (default: stderr).
void warn(std::string_view message);

}  // namespace vt::detail
