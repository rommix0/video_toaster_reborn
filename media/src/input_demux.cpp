// The decode thread: open a source, read its packets, decode its best video and audio streams,
// convert them and publish them on the session timeline. Opening is per source kind (a file, a
// DirectShow device, the desktop through gdigrab); the pump that turns packets into published
// pictures and sound is shared by all of them.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "input_internal.h"
#include "vt/log.h"
#include "vt/media/video_convert.h"

extern "C" {
#include <libavutil/mathematics.h>
}

namespace vt::media {
namespace {

using State = InputStatus::State;
using Steady = std::chrono::steady_clock;
using Options = std::map<std::string, std::string>;

constexpr int64_t kResyncSamples = 960;                // Ruling 8: 20 ms of drift before audio jumps to its pts
constexpr SessionTime kReanchor = 48000;               // 200 ms between a live timestamp and its arrival
constexpr auto kLiveSilence = std::chrono::seconds(2); // a live source quiet this long is reopened
constexpr int kLivePollMs = 3;                         // nap when no live source has a packet ready

// One decoded stream of an open source.
struct Stream {
    int index = -1;
    CodecCtxPtr dec;          // null: the source has no such stream (or it cannot be decoded)
    AVRational tb{0, 1};
    SessionTime frameDur = 0; // nominal video frame duration, for frames that carry none
};

// An open source: its demuxer and the decoders of its best video and audio streams. A stream
// timestamp ts is due at base + toSession(ts) - start.
struct Source {
    FormatCtxPtr fmt;
    Stream video, audio;
    int64_t startUs = 0;             // file: its first timestamp (AV_TIME_BASE units)
    SessionTime start = 0;           // file: the same in session units; live: 0
    SessionTime base = 0;            // file: when this pass starts; live: the device clock's offset
    bool anchored = false;           // live: base has been taken from a packet's arrival
    Steady::time_point lastPacket{}; // live: when it last delivered
};

#ifdef VT_MEDIA_TEST_SEAMS
bool failingForTest(const Input::Shared& s) { return s.cfg.failForTest && s.cfg.failForTest->load(); }
#else
constexpr bool failingForTest(const Input::Shared&) { return false; }
#endif

// Lets a blocked open or read give up when the input is being destroyed.
int interruptCb(void* opaque) { return static_cast<Input::Shared*>(opaque)->stopping.load() ? 1 : 0; }

// Finds the best stream of `type` and opens its decoder into `out`; the stream index or an error.
// A live stream decodes without frame threading, which would hold frames back.
int openStream(AVFormatContext* fmt, AVMediaType type, int related, bool live, Stream& out) {
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
    if (live) ctx->thread_type = FF_THREAD_SLICE;
    if ((err = avcodec_open2(ctx.get(), codec, nullptr)) < 0) return err;
    out.index = idx;
    out.dec = std::move(ctx);
    out.tb = st->time_base;
    const AVRational rate = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
    if (rate.num > 0 && rate.den > 0) out.frameDur = av_rescale(kSessionTb.den, rate.den, rate.num);
    return idx;
}

// Opens `url` (with the input format named `format`, or probed when null) and the decoders of
// its best streams; 0 or an FFmpeg error. A live source (format given) is opened non-blocking,
// because dshow's blocking read waits forever, deaf to the interrupt callback; and it is not
// probed: its parameters are known at open, and probing would only hold back its first packets.
int openSource(Input::Shared& s, Source& src, const std::string& url, const char* format, const Options& options) {
    const bool live = format != nullptr;
    const AVInputFormat* ifmt = nullptr;
    if (live && !(ifmt = av_find_input_format(format))) return AVERROR_DEMUXER_NOT_FOUND;
    AVFormatContext* raw = avformat_alloc_context();
    if (!raw) return AVERROR(ENOMEM);
    raw->interrupt_callback = {interruptCb, &s};
    if (live) raw->flags |= AVFMT_FLAG_NONBLOCK;
    AVDictionary* opts = nullptr;
    for (const auto& [k, v] : options) av_dict_set(&opts, k.c_str(), v.c_str(), 0);
    int err = avformat_open_input(&raw, url.c_str(), ifmt, &opts); // frees raw on failure
    av_dict_free(&opts);
    if (err < 0) return err;
    src.fmt.reset(raw);
    if (!live && (err = avformat_find_stream_info(raw, nullptr)) < 0) return err;

    const int v = openStream(raw, AVMEDIA_TYPE_VIDEO, -1, live, src.video);
    const int a = openStream(raw, AVMEDIA_TYPE_AUDIO, std::max(v, -1), live, src.audio);
    if (v < 0 && a < 0) return v != AVERROR_STREAM_NOT_FOUND ? v : a;
    if (v < 0 && v != AVERROR_STREAM_NOT_FOUND) logWarning(s.name + ": video skipped: " + averr(v));
    if (a < 0 && a != AVERROR_STREAM_NOT_FOUND) logWarning(s.name + ": audio skipped: " + averr(a));
    for (unsigned i = 0; i < raw->nb_streams; ++i)
        raw->streams[i]->discard = int(i) == v || int(i) == a ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    if (!live) {
        src.startUs = raw->start_time != AV_NOPTS_VALUE ? raw->start_time : 0;
        src.start = toSession(src.startUs, AV_TIME_BASE_Q);
    }
    return 0;
}

// Opens a media file (the File kind's source); 0 or an FFmpeg error.
int openFile(Input::Shared& s, Source& src) { return openSource(s, src, s.cfg.path, nullptr, s.cfg.options); }

// dshow's input string: "video=<name>[:audio=<name>]" or "audio=<name>". Names go in as they
// are (spaces included, no quotes); dshow splits the string at every ':' and has no escape, so
// a name holding one cannot be given (liveConfigError). Device ids never hold one.
std::string dshowUrl(const std::string& video, const std::string& audio) {
    std::string url;
    if (!video.empty()) url = "video=" + video;
    if (!audio.empty()) url += (url.empty() ? "audio=" : ":audio=") + audio;
    return url;
}

// Options for a dshow source: ours, overridden by `config`. A device's default audio buffer
// (often 500 ms) delivers sound in lumps that stray from the clock mapping, hence 50 ms.
Options dshowOptions(const std::string& audio, const Options& config) {
    Options o{{"rtbufsize", "100M"}};
    if (!audio.empty()) o["audio_buffer_size"] = "50";
    for (const auto& [k, v] : config) o[k] = v;
    return o;
}

// Opens a Device (one dshow source whose streams share one clock mapping) or a Screen (gdigrab,
// plus a dshow source for its audio device, with a mapping of its own); 0 or an FFmpeg error.
// The config's options go to the device or the screen, not to a screen's audio device.
int openLive(Input::Shared& s, std::vector<Source>& srcs) {
    const InputConfig& c = s.cfg;
    if (c.kind == InputKind::Device) {
        srcs.resize(1);
        return openSource(s, srcs[0], dshowUrl(c.videoDevice, c.audioDevice), "dshow",
                          dshowOptions(c.audioDevice, c.options));
    }
    srcs.resize(c.audioDevice.empty() ? 1 : 2);
    Options screen{{"framerate", "60000/1001"}, {"draw_mouse", "1"}};
    for (const auto& [k, v] : c.options) screen[k] = v;
    const std::string what = c.window.empty() ? "desktop" : "title=" + c.window;
    if (const int err = openSource(s, srcs[0], what, "gdigrab", screen); err < 0 || srcs.size() == 1) return err;
    return openSource(s, srcs[1], dshowUrl({}, c.audioDevice), "dshow", dshowOptions(c.audioDevice, {}));
}

// Back to the start of a file for the next loop; false if it cannot seek.
bool rewind(Source& src) { return av_seek_frame(src.fmt.get(), -1, src.startUs, AVSEEK_FLAG_BACKWARD) >= 0; }

int64_t frameTs(const AVFrame& f) { return f.best_effort_timestamp != AV_NOPTS_VALUE ? f.best_effort_timestamp : f.pts; }

// Turns a source's packets into published pictures and sound. Lives as long as the thread, so
// its converters and the once-per-input interlace warning carry across reopens and loops.
class Pump {
public:
    explicit Pump(Input::Shared& s) : s_(s), vconv_(s.geo), frame_(av_frame_alloc()), pkt_(av_packet_alloc()) {}

    // Plays a file `src` from where it stands to its end, its first timestamp due at `base`.
    // False when the input is stopping or the source failed (failure() says why; "" when
    // stopping); length() is then the session time the pass covered (video or audio, whichever
    // ends later), which is how far the next loop starts after this one.
    bool play(Source& src, SessionTime base) {
        begin();
        src.base = base;
        for (;;) {
            if (!s_.waitForRoom()) return false;
            if (failingForTest(s_)) {
                failure_ = "simulated failure";
                return false;
            }
            const int err = av_read_frame(src.fmt.get(), pkt_.get());
            if (err < 0) {
                if (s_.stopping) return false;
                if (err != AVERROR_EOF) logWarning(s_.name + ": read failed, ending here: " + averr(err));
                break;
            }
            route(src, *pkt_);
            av_packet_unref(pkt_.get());
        }
        // Drain what the decoders still hold, then reset them for a rewind.
        if (src.video.dec) decode(src, src.video, nullptr, true);
        if (src.audio.dec) decode(src, src.audio, nullptr, false);
        if (src.video.dec) avcodec_flush_buffers(src.video.dec.get());
        if (src.audio.dec) avcodec_flush_buffers(src.audio.dec.get());
        if (length_ > 0) s_.passComplete(src.base + length_);
        return !s_.stopping;
    }

    SessionTime length() const { return length_; }
    const std::string& failure() const { return failure_; }

    // Captures from open live sources as fast as they deliver, never waiting for the reader (a
    // full picture queue drops its oldest picture instead). Returns why it stopped: a read error,
    // or a source that delivered nothing for 2 s; "" when the input is stopping.
    // Each round takes what a source has ready up to one video packet, so a slow screen grab
    // cannot leave its audio device's packets waiting (their arrival times feed the clock mapping).
    std::string capture(std::vector<Source>& srcs) {
        begin();
        for (Source& src : srcs) src.lastPacket = Steady::now();
        for (;;) {
            if (failingForTest(s_)) return "simulated failure";
            bool got = false;
            for (Source& src : srcs) {
                for (bool video = false; !video;) {
                    const int err = av_read_frame(src.fmt.get(), pkt_.get());
                    if (s_.stopping) return {};
                    if (err == AVERROR(EAGAIN)) break; // nothing ready yet
                    if (err < 0) return err == AVERROR_EOF ? "capture stopped" : averr(err);
                    got = true;
                    video = pkt_->stream_index == src.video.index;
                    src.lastPacket = Steady::now();
                    anchor(src, *pkt_);
                    route(src, *pkt_);
                    av_packet_unref(pkt_.get());
                }
            }
            const auto now = Steady::now();
            for (const Source& src : srcs)
                if (now - src.lastPacket > kLiveSilence) return "nothing delivered for 2 s";
            if (!got && !s_.sleepFor(kLivePollMs)) return {};
        }
    }

private:
    // A new pass or capture: timing and audio placement start over.
    void begin() {
        width_ = height_ = -1; // setRunning reported the opened stream's size, which may be unknown
        failure_.clear();
        length_ = 0;
        lastVideo_ = 0;
        synced_ = false;
    }

    // Live timestamps run on the device's clock: the first packet's arrival on the session clock
    // fixes the mapping, and a packet more than 200 ms off it moves the mapping (logged).
    void anchor(Source& src, const AVPacket& pkt) {
        if (pkt.pts == AV_NOPTS_VALUE || (pkt.stream_index != src.video.index && pkt.stream_index != src.audio.index))
            return;
        const SessionTime arrival = s_.clock.now();
        const SessionTime ts = toSession(pkt.pts, src.fmt->streams[pkt.stream_index]->time_base);
        const SessionTime off = src.base + ts - arrival;
        if (src.anchored && std::abs(off) <= kReanchor) return;
        if (src.anchored)
            logWarning(s_.name + ": live clock re-anchored (timestamps were " + std::to_string(off / 240) +
                       " ms off their arrival)");
        src.base = arrival - ts;
        src.anchored = true;
    }

    void route(const Source& src, const AVPacket& pkt) {
        if (pkt.stream_index == src.video.index) decode(src, src.video, &pkt, true);
        else if (pkt.stream_index == src.audio.index) decode(src, src.audio, &pkt, false);
    }

    // Sends one packet (null: end of stream) and handles every frame it releases. A damaged
    // packet or frame is counted and skipped.
    void decode(const Source& src, const Stream& st, const AVPacket* pkt, bool video) {
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
            if (video) onVideo(src, st, *frame_);
            else onAudio(src, *frame_, st.tb);
            av_frame_unref(frame_.get());
        }
    }

    void onVideo(const Source& src, const Stream& st, const AVFrame& f) {
        if (!warnedInterlaced_ && (f.flags & AV_FRAME_FLAG_INTERLACED)) {
            warnedInterlaced_ = true;
            logWarning(s_.name + ": interlaced video, shown as progressive frames");
        }
        if (f.width != width_ || f.height != height_) s_.setPictureSize(width_ = f.width, height_ = f.height);
        const int64_t ts = frameTs(f);
        const SessionTime dur = f.duration > 0 ? toSession(f.duration, st.tb) : st.frameDur;
        const SessionTime rel = ts != AV_NOPTS_VALUE ? toSession(ts, st.tb) - src.start : lastVideo_ + dur;
        lastVideo_ = rel;
        length_ = std::max(length_, rel + dur);
        vt::Frame pic = s_.takeBuffer();
        vconv_.convert(f, pic);
        s_.publishPicture(src.base + rel, std::move(pic));
    }

    // Ruling 8: chunks are laid end to end (the resampler's output count wobbles by a sample),
    // jumping to the timestamp only after a rewind or reopen or when it drifts too far (as it
    // does after a live re-anchor).
    void onAudio(const Source& src, const AVFrame& f, AVRational tb) {
        const int64_t ts = frameTs(f);
        const SessionTime rel = ts != AV_NOPTS_VALUE ? toSession(ts, tb) - src.start : AV_NOPTS_VALUE;
        if (rel != AV_NOPTS_VALUE && f.sample_rate > 0)
            length_ = std::max(length_, rel + av_rescale(f.nb_samples, kSessionTb.den, f.sample_rate));
        const std::span<const float> pcm = aconv_.convert(f);
        if (pcm.empty()) return;
        if (rel != AV_NOPTS_VALUE) {
            const int64_t at = floorDiv(src.base + rel, 5);
            if (!synced_ || std::abs(at - next_) > kResyncSamples) next_ = at;
            synced_ = true;
        } else if (!synced_) {
            next_ = floorDiv(src.base, 5);
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
    int width_ = -1, height_ = -1; // the picture size last reported
    std::string failure_;          // why the last play() failed
    SessionTime length_ = 0, lastVideo_ = 0;
    bool synced_ = false; // next_ holds a position from this pass
    int64_t next_ = 0;    // where the next audio chunk goes (48 kHz sample index)
};

// Plays an opened file until it ends (loop off), fails, or the input stops. Returns the error
// text when the input should retry, or "" when the thread is done.
std::string playFile(Input::Shared& s, Pump& pump, Source& src) {
    // The first tick boundary from now on: in real time the first picture and the first sample
    // then land on the same tick (offline the clock already stands on one).
    SessionTime base = -floorDiv(-s.clock.now(), tickTime(1)) * tickTime(1);
    for (;;) {
        if (!pump.play(src, base)) return pump.failure(); // "" when stopping
        if (pump.length() <= 0) return "no playable frames";
        if (!s.cfg.loop) {
            s.setState(State::Ended, s.name);
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

std::string describeInput(const InputConfig& c) {
    switch (c.kind) {
    case InputKind::None: return {};
    case InputKind::File: return c.path;
    case InputKind::Device: return dshowUrl(c.videoDevice, c.audioDevice);
    case InputKind::Screen:
        return (c.window.empty() ? std::string("desktop") : "window " + c.window) +
               (c.audioDevice.empty() ? "" : " + audio=" + c.audioDevice);
    }
    return {};
}

std::string liveConfigError(const InputConfig& c) {
    const auto colon = [](const std::string& name) { return name.find(':') != std::string::npos; };
    if (c.kind == InputKind::Device && c.videoDevice.empty() && c.audioDevice.empty()) return "no device given";
    if (c.kind == InputKind::Device && colon(c.videoDevice))
        return c.videoDevice + ": DirectShow cannot take a name with ':' (use the device id)";
    if (colon(c.audioDevice)) return c.audioDevice + ": DirectShow cannot take a name with ':' (use the device id)";
    return {};
}

void runInput(Input::Shared& s) {
    initMedia();
    Pump pump(s);
    while (!s.stopping) {
        s.setOpening();
        std::vector<Source> srcs(1); // a file or a device: one; a screen with an audio device: two
        std::string failure;
        if (const int err = s.live ? openLive(s, srcs) : openFile(s, srcs[0]); err < 0) {
            failure = averr(err);
        } else {
            const Stream* video = nullptr;
            bool audio = false;
            for (const Source& src : srcs) {
                if (!video && src.video.dec) video = &src.video;
                audio = audio || src.audio.dec != nullptr;
            }
            s.setRunning(video ? video->dec->width : 0, video ? video->dec->height : 0, video != nullptr, audio);
            failure = s.live ? pump.capture(srcs) : playFile(s, pump, srcs[0]);
            if (failure.empty()) return; // ended, or stopping
        }
        if (s.stopping) return;
        s.setState(State::Error, s.name + ": " + failure);
        if (!s.sleepFor(s.cfg.retryMs)) return;
    }
}

} // namespace vt::media
