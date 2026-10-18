#include "vt/log.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

#include "log_detail.h"

namespace vt {
namespace {

std::mutex& sinkMutex() {
    static std::mutex m;
    return m;
}

std::function<void(std::string_view)>& sink() {
    static std::function<void(std::string_view)> s;
    return s;
}

}  // namespace

void setLogSink(std::function<void(std::string_view)> s) {
    std::lock_guard<std::mutex> lock(sinkMutex());
    sink() = std::move(s);
}

namespace detail {

void warn(std::string_view message) {
    std::lock_guard<std::mutex> lock(sinkMutex());
    if (sink()) {
        sink()(message);
        return;
    }
    std::fprintf(stderr, "vtoast: %.*s\n", int(message.size()), message.data());
}

}  // namespace detail
}  // namespace vt
