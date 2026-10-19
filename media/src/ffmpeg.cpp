#include "vt/media/ffmpeg.h"

#include <cstdarg>
#include <mutex>
#include <string_view>

#include "vt/log.h"

namespace vt::media {
namespace {

void logCallback(void* avcl, int level, const char* fmt, va_list args) {
    if (level > AV_LOG_WARNING) return;  // FFmpeg levels grow less severe: keep warning and worse
    char line[1024];
    int printPrefix = 1;  // per call: every message is formatted whole, so each line gets its prefix
    av_log_format_line2(avcl, level, fmt, args, line, sizeof line, &printPrefix);
    std::string_view text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    if (!text.empty()) logWarning(text);
}

}  // namespace

std::string averr(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buf, sizeof buf);
    return buf;
}

void initMedia() {
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_callback(logCallback);
        avdevice_register_all();
    });
}

}  // namespace vt::media
