// The decode thread: open a source, read its packets, decode its best video and audio streams,
// convert them and publish them on the session timeline. Opening is per source kind; the pump
// that turns packets into published pictures and sound is shared by all of them.
#include <algorithm>
#include <cstdlib>
#include <string>

#include "input_internal.h"
#include "vt/log.h"
#include "vt/media/video_convert.h"

extern "C" {
#include <libavutil/mathematics.h>
}

namespace vt::media {
namespace {

using State = InputStatus::State;

constexpr int64_t kResyncSamples = 960; // Ruling 8: 20 ms of drift before audio jumps to its pts

// One decoded stream of an open source.
struct Stream {
    int index = -1;
    CodecCtxPtr dec;          // null: the source has no such stream (or it cannot be decoded)
    AVRational tb{0, 1};
    SessionTime frameDur = 0; // nominal video frame duration, for frames that carry none
};

// An open source: its demuxer and the decoders of its best video and audio streams.
struct Source {
    FormatCtxPtr fmt;
    Stream video, audio;
    int64_t startUs = 0;      // the source's first timestamp (AV_TIME_BASE units)
    SessionTime start = 0;    // the same in session units
};

// Lets a blocked open or read give up when the input is being destroyed.
int interruptCb(void* opaque) { return static_cast<Input::Shared*>(opaque)->stopping.load() ? 1 : 0; }

// Finds the best stream of `type` and opens its decoder into `out`; the stream index or an error.
int openStream(AVFormatContext* fmt, AVMediaType type, int related, Stream& out) {
    const AVCodec* codec = nullptr;
    const int idx = av_find_best_stream(fmt, type, -1, related, &codec, 0);
    if (idx < 0) return idx;
    const AVStream* st = fmt->streams[idx];
    CodecCtxPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx) return AVERROR(ENOMEM);
    int err = avcodec_parameters_to_context(ctx.get(), st->codecpar);
    if (err < 0) return err;
    ctx->pkt_timebase = st->time_base;
    ctx->thread_count = 0; // one per core, frame threading where the codec has it
    if ((err = avcodec_open2(ctx.get(), codec, nullptr)) < 0) return err;
    out.index = idx;
    out.dec = std::move(ctx);
    out.tb = st->time_base;
    const AVRational rate = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
    if (rate.num > 0 && rate.den > 0) out.frameDur = av_rescale(kSessionTb.den, rate.den, rate.num);
    return idx;
}

// Opens a media file (the File kind's source); 0 or an FFmpeg error.
int openFile(Input::Shared& s, Source& src) {
    AVFormatContext* raw = avformat_alloc_context();
    if (!raw) return AVERROR(ENOMEM);
    raw->interrupt_callback = {interruptCb, &s};
    AVDictionary* opts = nullptr;
    for (const auto& [k, v] : s.cfg.options) av_dict_set(&opts, k.c_str(), v.c_str(), 0);
    int err = avformat_open_input(&raw, s.cfg.path.c_str(), nullptr, &opts); // frees raw on failure
    av_dict_free(&opts);
    if (err < 0) return err;
    src.fmt.reset(raw);
    if ((err = avformat_find_stream_info(raw, nullptr)) < 0) return err;

    const int v = openStream(raw, AVMEDIA_TYPE_VIDEO, -1, src.video);
    const int a = openStream(raw, AVMEDIA_TYPE_AUDIO, std::max(v, -1), src.audio);
    if (v < 0 && a < 0) return v != AVERROR_STREAM_NOT_FOUND ? v : a;
    if (v < 0 && v != AVERROR_STREAM_NOT_FOUND) logWarning(s.cfg.path + ": video skipped: " + averr(v));
    if (a < 0 && a != AVERROR_STREAM_NOT_FOUND) logWarning(s.cfg.path + ": audio skipped: " + averr(a));
    for (unsigned i = 0; i < raw->nb_streams; ++i)
        raw->streams[i]->discard = int(i) == v || int(i) == a ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    src.startUs = raw->start_time != AV_NOPTS_VALUE ? raw->start_time : 0;
    src.start = toSession(src.startUs, AV_TIME_BASE_Q);
    return 0;
}

// Back to the start of a file for the next loop; false if it cannot seek.
bool rewind(Source& src) { return av_seek_frame(src.fmt.get(), -1, src.startUs, AVSEEK_FLAG_BACKWARD) >= 0; }

int64_t frameTs(const AVFrame& f) { return f.best_effort_timestamp != AV_NOPTS_VALUE ? f.best_effort_timestamp : f.pts; }

// Turns a source's packets into published pictures and sound. Lives as long as the thread, so
// its converters and the once-per-input interlace warning carry across reopens and loops.
class Pump {
public:
    explicit Pump(Input::Shared& s) : s_(s), vconv_(s.geo), frame_(av_frame_alloc()), pkt_(av_packet_alloc()) {}

    // Plays `src` from where it stands to its end, its first timestamp due at `base`. False when
    // the input is stopping; length() is then the session time the pass covered (video or
    // audio, whichever ends later), which is how far the next loop starts after this one.
    bool play(Source& src, SessionTime base) {
        base_ = base;
        start_ = src.start;
        length_ = 0;
        lastVideo_ = 0;
        synced_ = false;
        for (;;) {
            if (!s_.waitForRoom()) return false;
            const int err = av_read_frame(src.fmt.get(), pkt_.get());
            if (err < 0) {
                if (s_.stopping) return false;
                if (err != AVERROR_EOF) logWarning(s_.cfg.path + ": read failed, ending here: " + averr(err));
                break;
            }
            if (pkt_->stream_index == src.video.index) decode(src.video, pkt_.get(), true);
            else if (pkt_->stream_index == src.audio.index) decode(src.audio, pkt_.get(), false);
            av_packet_unref(pkt_.get());
        }
        // Drain what the decoders still hold, then reset them for a rewind.
        if (src.video.dec) decode(src.video, nullptr, true);
        if (src.audio.dec) decode(src.audio, nullptr, false);
        if (src.video.dec) avcodec_flush_buffers(src.video.dec.get());
        if (src.audio.dec) avcodec_flush_buffers(src.audio.dec.get());
        if (length_ > 0) s_.videoCompleteThrough(base_ + length_ - 1);
        return !s_.stopping;
    }

    SessionTime length() const { return length_; }

private:
    // Sends one packet (null: end of stream) and handles every frame it releases. A damaged
    // packet or frame is counted and skipped.
    void decode(Stream& st, const AVPacket* pkt, bool video) {
        int err = avcodec_send_packet(st.dec.get(), pkt);
        if (err < 0 && err != AVERROR_EOF) {
            s_.countDecodeError();
            if (pkt) return;
        }
        for (;;) {
            err = avcodec_receive_frame(st.dec.get(), frame_.get());
            if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) return;
            if (err < 0) {
                s_.countDecodeError();
                return;
            }
            if (video) onVideo(st, *frame_);
            else onAudio(*frame_, st.tb);
            av_frame_unref(frame_.get());
        }
    }

    void onVideo(const Stream& st, const AVFrame& f) {
        if (!warnedInterlaced_ && (f.flags & AV_FRAME_FLAG_INTERLACED)) {
            warnedInterlaced_ = true;
            logWarning(s_.cfg.path + ": interlaced video, shown as progressive frames");
        }
        const int64_t ts = frameTs(f);
        const SessionTime dur = f.duration > 0 ? toSession(f.duration, st.tb) : st.frameDur;
        const SessionTime rel = ts != AV_NOPTS_VALUE ? toSession(ts, st.tb) - start_ : lastVideo_ + dur;
        lastVideo_ = rel;
        length_ = std::max(length_, rel + dur);
        vt::Frame pic = s_.takeBuffer();
        vconv_.convert(f, pic);
        s_.publishPicture(base_ + rel, std::move(pic));
    }

    // Ruling 8: chunks are laid end to end (the resampler's output count wobbles by a sample),
    // jumping to the timestamp only after a rewind or reopen or when it drifts too far.
    void onAudio(const AVFrame& f, AVRational tb) {
        const int64_t ts = frameTs(f);
        const SessionTime rel = ts != AV_NOPTS_VALUE ? toSession(ts, tb) - start_ : AV_NOPTS_VALUE;
        if (rel != AV_NOPTS_VALUE && f.sample_rate > 0)
            length_ = std::max(length_, rel + av_rescale(f.nb_samples, kSessionTb.den, f.sample_rate));
        const std::span<const float> pcm = aconv_.convert(f);
        if (pcm.empty()) return;
        if (rel != AV_NOPTS_VALUE) {
            const int64_t at = floorDiv(base_ + rel, 5);
            if (!synced_ || std::abs(at - next_) > kResyncSamples) next_ = at;
            synced_ = true;
        } else if (!synced_) {
            next_ = floorDiv(base_, 5);
            synced_ = true;
        }
        s_.publishAudio(next_, pcm);
        next_ += int64_t(pcm.size() / 2);
    }

    Input::Shared& s_;
    VideoConverter vconv_;
    AudioConverter aconv_;
    FramePtr frame_;
    PacketPtr pkt_;
    bool warnedInterlaced_ = false;
    SessionTime base_ = 0, start_ = 0, length_ = 0, lastVideo_ = 0;
    bool synced_ = false; // next_ holds a position from this pass
    int64_t next_ = 0;    // where the next audio chunk goes (48 kHz sample index)
};

// Plays an opened file until it ends (loop off), fails, or the input stops. Returns the error
// text when the input should retry, or "" when the thread is done.
std::string playFile(Input::Shared& s, Pump& pump, Source& src) {
    SessionTime base = s.clock.now();
    for (;;) {
        if (!pump.play(src, base)) return {};
        if (pump.length() <= 0) return "no playable frames";
        if (!s.cfg.loop) {
            s.setState(State::Ended, s.cfg.path);
            return {};
        }
        base += pump.length();
        if (!rewind(src)) {
            src = Source{};
            if (const int err = openFile(s, src); err < 0) return averr(err);
        }
    }
}

} // namespace

void runInput(Input::Shared& s) {
    initMedia();
    Pump pump(s);
    while (!s.stopping) {
        s.setOpening();
        Source src;
        std::string failure;
        if (const int err = openFile(s, src); err < 0) {
            failure = averr(err);
        } else {
            s.setRunning(src.video.dec ? src.video.dec->width : 0, src.video.dec ? src.video.dec->height : 0,
                         src.video.dec != nullptr, src.audio.dec != nullptr);
            failure = playFile(s, pump, src);
            if (failure.empty()) return; // ended, or stopping
        }
        if (s.stopping) return;
        s.setState(State::Error, s.cfg.path + ": " + failure);
        if (!s.sleepFor(s.cfg.retryMs)) return;
    }
}

} // namespace vt::media
