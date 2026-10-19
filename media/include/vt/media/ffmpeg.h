#pragma once
// Thin FFmpeg helpers shared by the media library: RAII owners for the common FFmpeg objects,
// error strings, and one-time global setup. FFmpeg's headers are C, so they sit in extern "C".
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace vt::media {

struct CodecCtxDeleter { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
// An input context: closing it also frees the context (avformat_close_input).
struct FormatCtxDeleter { void operator()(AVFormatContext* c) const { avformat_close_input(&c); } };
struct FrameDeleter { void operator()(AVFrame* f) const { av_frame_free(&f); } };
struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct SwsDeleter { void operator()(SwsContext* s) const { sws_free_context(&s); } };
struct SwrDeleter { void operator()(SwrContext* s) const { swr_free(&s); } };

using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;
using FormatCtxPtr = std::unique_ptr<AVFormatContext, FormatCtxDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;

// The text for an FFmpeg error code (AVERROR(...) or an AVERROR_* tag).
std::string averr(int errnum);

// Registers FFmpeg's capture devices (dshow, gdigrab, ...) and routes its log lines of level
// AV_LOG_WARNING and above to vt::logWarning. Safe to call from any thread, any number of times.
void initMedia();

}  // namespace vt::media
