#include "catch_amalgamated.hpp"
#include "vt/media/ffmpeg.h"

#include <cerrno>
#include <filesystem>

TEST_CASE("FFmpeg DLLs match the headers") {
    vt::media::initMedia();
    CHECK(avcodec_version() >> 16 == LIBAVCODEC_VERSION_MAJOR);   // 63
    CHECK(avformat_version() >> 16 == LIBAVFORMAT_VERSION_MAJOR); // 63
    CHECK(avutil_version() >> 16 == LIBAVUTIL_VERSION_MAJOR);     // 61
    CHECK(swscale_version() >> 16 == LIBSWSCALE_VERSION_MAJOR);   // 10
    CHECK(swresample_version() >> 16 == LIBSWRESAMPLE_VERSION_MAJOR); // 7
    for (const char* e : {"ffv1", "prores_ks", "libx264", "aac", "pcm_s16le"})
        CHECK(avcodec_find_encoder_by_name(e) != nullptr);
    CHECK(av_find_input_format("dshow") != nullptr);
    CHECK(av_find_input_format("gdigrab") != nullptr);
}
TEST_CASE("fixtures exist") {
    for (const char* f : {"f30.mkv", "f25_169.mkv", "f60_sar.mkv"})
        CHECK(std::filesystem::file_size(std::filesystem::path(VT_MEDIA_FIXTURES) / f) > 1000);
}
TEST_CASE("averr names an error") { CHECK(vt::media::averr(AVERROR(ENOENT)).find("No such file") != std::string::npos); }
