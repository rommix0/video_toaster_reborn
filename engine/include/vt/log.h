#pragma once
// Where the engine's warnings go: effects that load with something missing (an undecodable
// sound, unknown DVE fill codes). The engine writes to stderr only through the default sink.
#include <functional>
#include <string_view>

namespace vt {

// Replaces the sink that receives each warning (one line, no trailing newline). An empty
// function restores the default, which writes "vtoast: <warning>\n" to stderr. Warnings may come
// from any thread that loads effects; the sink is called under a lock, one warning at a time,
// so it must not call setLogSink itself.
void setLogSink(std::function<void(std::string_view)>);

}  // namespace vt
