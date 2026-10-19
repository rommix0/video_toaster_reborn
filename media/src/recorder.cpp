#include "vt/media/recorder.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "vt/media/ffmpeg.h"
#include "vt/media/time.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace vt::media {
namespace {

constexpr int kQueueCap = 120;      // frames waiting for the encoder
constexpr int kAudioChunk = 1024;   // samples per audio frame (AAC's fixed frame size)
constexpr AVRational kTick{1001, 60000};  // one tick; the video time base

// An output context owns its file handle too.
struct OutCtxDeleter {
    void operator()(AVFormatContext* c) const {
        if (!c) return;
        if (c->pb && !(c->oformat->flags & AVFMT_NOFILE)) avio_closep(&c->pb);
        avformat_free_context(c);
    }
};
using OutCtxPtr = std::unique_ptr<AVFormatContext, OutCtxDeleter>;

struct AvDict {
    AVDictionary* d = nullptr;
    ~AvDict() { av_dict_free(&d); }
    void set(const char* k, const std::string& v) { av_dict_set(&d, k, v.c_str(), 0); }
};

// One tick's worth of copied input. Entries are recycled through Impl::freeList.
struct Entry {
    int64_t pts = 0;            // ticks since the first recorded tick
    bool hasVideo = false;      // false: the frame was dropped, only the audio is kept
    std::vector<uint8_t> px;    // the crop, RGBA
    std::vector<float> audio;   // interleaved stereo
};

const char* muxerName(RecordFormat f) {
    switch (f) {
        case RecordFormat::H264Mkv: case RecordFormat::Ffv1Mkv: return "matroska";
        case RecordFormat::H264Mp4: return "mp4";
        case RecordFormat::ProResMov: return "mov";
    }
    return "matroska";
}

} // namespace

struct Recorder::Impl {
    RecorderSettings settings;
    vt::Rect rect{};

    std::mutex startMu;                 // one start() at a time

    // Shared between the loop thread, stop() and the encoder thread.
    std::mutex mu;
    std::condition_variable cvWork;     // the encoder waits: an entry is queued, or stopping
    std::condition_variable cvSpace;    // a blocked producer waits: room in the queue, or not accepting
    std::condition_variable cvStopped;  // start() and other stop() calls wait: the joining stop() is done
    bool joining = false;               // a stop() has taken the thread and is joining it
    std::deque<std::unique_ptr<Entry>> queue;
    std::vector<std::unique_ptr<Entry>> freeList;
    int queuedVideo = 0;                // queue entries that carry a picture
    bool accepting = false, stopping = false, paused = false, haveFirst = false;
    int64_t firstTick = 0;
    RecorderStatus status;
    std::thread thread;

    // Owned by the encoder thread once it runs.
    OutCtxPtr out;
    CodecCtxPtr vctx, actx;
    AVStream* vst = nullptr;
    AVStream* ast = nullptr;
    SwsPtr sws;
    FramePtr vframe, srcView, aframe;
    PacketPtr pkt;
    std::vector<float> pending;         // audio not yet encoded
    size_t pendingPos = 0;              // in floats
    int64_t audioPts = 0;
    bool headerWritten = false, failed = false, rgb = false;
#ifdef VT_MEDIA_TEST_SEAMS
    int64_t bytesWritten = 0;
#endif

    bool open(const RecorderSettings&, std::string& err);
    void run();
    void fail(const std::string& what);
    bool encodeEntry(const Entry&);
    bool encodeAudio(int samples);
    bool send(AVCodecContext*, AVFrame*);
    bool writePackets(AVCodecContext*, AVStream*, bool video);
    void finish();
    std::unique_ptr<Entry> takeEntry();
    void onProgram(int64_t tick, const vt::Frame&, std::span<const float>);
};

bool Recorder::Impl::open(const RecorderSettings& s, std::string& err) {
    const vt::Rect r = s.geometry.recordRect;
    if (r.w <= 0 || r.h <= 0 || (r.w & 1) || (r.h & 1) || r.x < 0 || r.y < 0 || r.x + r.w > vt::kRasterW ||
        r.y + r.h > vt::kRasterH) {
        err = "invalid record rectangle";
        return false;
    }
    if (s.path.empty()) { err = "no output path"; return false; }
    rect = r;
    settings = s;
    initMedia();

    const bool h264 = s.format == RecordFormat::H264Mkv || s.format == RecordFormat::H264Mp4;
    rgb = s.format == RecordFormat::Ffv1Mkv;
    const char* vname = h264 ? "libx264" : rgb ? "ffv1" : "prores_ks";
    const char* aname = h264 ? "aac" : "pcm_s16le";
    const AVPixelFormat pix = h264 ? AV_PIX_FMT_YUV420P : rgb ? AV_PIX_FMT_BGR0 : AV_PIX_FMT_YUV422P10LE;

    const auto u8 = s.path.u8string();
    const std::string path(u8.begin(), u8.end());
    AVFormatContext* raw = nullptr;
    int rc = avformat_alloc_output_context2(&raw, nullptr, muxerName(s.format), path.c_str());
    if (rc < 0) { err = "cannot create the output: " + averr(rc); return false; }
    out.reset(raw);
    const bool globalHeader = out->oformat->flags & AVFMT_GLOBALHEADER;

    // Video.
    const AVCodec* vc = avcodec_find_encoder_by_name(vname);
    if (!vc) { err = std::string("encoder not found: ") + vname; return false; }
    vctx.reset(avcodec_alloc_context3(vc));
    if (!vctx) { err = "out of memory"; return false; }
    vctx->width = r.w;
    vctx->height = r.h;
    vctx->pix_fmt = pix;
    vctx->time_base = kTick;
    vctx->framerate = {kTick.den, kTick.num};
    vctx->sample_aspect_ratio = s.geometry.pixelAspect;
    vctx->field_order = AV_FIELD_PROGRESSIVE;
    vctx->color_primaries = AVCOL_PRI_SMPTE170M;
    vctx->color_trc = AVCOL_TRC_SMPTE170M;
    // RGB carries no matrix and spans the full range; the YUV formats are BT.601 limited range.
    vctx->colorspace = rgb ? AVCOL_SPC_RGB : AVCOL_SPC_SMPTE170M;
    vctx->color_range = rgb ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    if (globalHeader) vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    // One thread per core (FFV1 and ProRes encode slices in parallel; libx264 picks its own
    // count). The default of one thread leaves ProRes below real time.
    vctx->thread_count = 0;
    AvDict vopts;
    if (h264) {
        vopts.set("preset", "veryfast");
        vopts.set("crf", std::to_string(s.crf));
        vopts.set("profile", "high");
    } else if (rgb) {
        vopts.set("level", "3");
        vopts.set("slicecrc", "1");
    } else {
        vopts.set("profile", "3");  // 422 HQ
    }
    rc = avcodec_open2(vctx.get(), vc, &vopts.d);
    if (rc < 0) { err = std::string("cannot open ") + vname + ": " + averr(rc); return false; }
    vst = avformat_new_stream(out.get(), nullptr);
    if (!vst) { err = "out of memory"; return false; }
    avcodec_parameters_from_context(vst->codecpar, vctx.get());
    vst->time_base = kTick;
    vst->avg_frame_rate = vst->r_frame_rate = vctx->framerate;
    vst->sample_aspect_ratio = vctx->sample_aspect_ratio;

    // Audio.
    const AVCodec* ac = avcodec_find_encoder_by_name(aname);
    if (!ac) { err = std::string("encoder not found: ") + aname; return false; }
    actx.reset(avcodec_alloc_context3(ac));
    if (!actx) { err = "out of memory"; return false; }
    actx->sample_rate = 48000;
    actx->sample_fmt = h264 ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_S16;
    av_channel_layout_default(&actx->ch_layout, 2);
    actx->time_base = {1, 48000};
    if (h264) actx->bit_rate = int64_t(s.audioKbps) * 1000;
    if (globalHeader) actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    rc = avcodec_open2(actx.get(), ac, nullptr);
    if (rc < 0) { err = std::string("cannot open ") + aname + ": " + averr(rc); return false; }
    ast = avformat_new_stream(out.get(), nullptr);
    if (!ast) { err = "out of memory"; return false; }
    avcodec_parameters_from_context(ast->codecpar, actx.get());
    ast->time_base = actx->time_base;

    // Frames, converter, packet.
    vframe.reset(av_frame_alloc());
    srcView.reset(av_frame_alloc());
    aframe.reset(av_frame_alloc());
    pkt.reset(av_packet_alloc());
    if (!vframe || !srcView || !aframe || !pkt) { err = "out of memory"; return false; }
    vframe->format = pix;
    vframe->width = r.w;
    vframe->height = r.h;
    rc = av_frame_get_buffer(vframe.get(), 0);
    if (rc < 0) { err = "out of memory: " + averr(rc); return false; }
    aframe->format = actx->sample_fmt;
    aframe->sample_rate = actx->sample_rate;
    aframe->nb_samples = kAudioChunk;
    av_channel_layout_copy(&aframe->ch_layout, &actx->ch_layout);
    rc = av_frame_get_buffer(aframe.get(), 0);
    if (rc < 0) { err = "out of memory: " + averr(rc); return false; }
    if (!rgb) {
        // RGB0 (the alpha byte is ignored) -> YUV at the same size: only the colour changes.
        srcView->format = AV_PIX_FMT_RGB0;
        srcView->width = r.w;
        srcView->height = r.h;
        srcView->colorspace = AVCOL_SPC_RGB;
        srcView->color_range = AVCOL_RANGE_JPEG;
        srcView->linesize[0] = r.w * 4;
        sws.reset(sws_alloc_context());
        if (!sws) { err = "out of memory"; return false; }
        sws->flags = SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    }

    // The file.
    if (!(out->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&out->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) { err = "cannot open " + path + ": " + averr(rc); return false; }
    }
    rc = avformat_write_header(out.get(), nullptr);
    if (rc < 0) { err = "cannot write the header: " + averr(rc); return false; }
    headerWritten = true;
    return true;
}

void Recorder::Impl::fail(const std::string& what) {
    failed = true;
    std::lock_guard lk(mu);
    if (status.error.empty()) status.error = what;
    status.recording = false;
    accepting = false;
    cvSpace.notify_all();
}

std::unique_ptr<Entry> Recorder::Impl::takeEntry() {
    if (freeList.empty()) return std::make_unique<Entry>();
    auto e = std::move(freeList.back());
    freeList.pop_back();
    return e;
}

void Recorder::Impl::onProgram(int64_t tick, const vt::Frame& program, std::span<const float> stereo) {
    std::unique_lock lk(mu);
    if (!accepting) return;
    if (!haveFirst) { firstTick = tick; haveFirst = true; }
    if (tick < firstTick) return;
    bool video = true;
    if (queuedVideo >= kQueueCap) {
        if (settings.blockWhenFull) {
            cvSpace.wait(lk, [&] { return queuedVideo < kQueueCap || !accepting; });
            if (!accepting) return;
        } else {
            video = false;
            ++status.framesDropped;
        }
    }
    auto e = takeEntry();
    e->pts = tick - firstTick;
    e->hasVideo = video;
    if (video) {
        static_assert(sizeof(vt::Rgba8) == 4);
        const size_t rowBytes = size_t(rect.w) * 4;
        e->px.resize(rowBytes * size_t(rect.h));
        for (int y = 0; y < rect.h; ++y)
            std::memcpy(e->px.data() + size_t(y) * rowBytes, &program.at(rect.x, rect.y + y), rowBytes);
        ++queuedVideo;
    }
    e->audio.assign(stereo.begin(), stereo.end());
    queue.push_back(std::move(e));
    cvWork.notify_one();
}

// Hands one frame (null: flush) to the encoder.
bool Recorder::Impl::send(AVCodecContext* c, AVFrame* f) {
    const int rc = avcodec_send_frame(c, f);
    if (rc < 0 && rc != AVERROR_EOF) {
        fail("encoder failed: " + averr(rc));
        return false;
    }
    return writePackets(c, c == vctx.get() ? vst : ast, c == vctx.get());
}

bool Recorder::Impl::writePackets(AVCodecContext* c, AVStream* st, bool video) {
    for (;;) {
        int rc = avcodec_receive_packet(c, pkt.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
        if (rc < 0) { fail("encoder failed: " + averr(rc)); return false; }
        pkt->stream_index = st->index;
        av_packet_rescale_ts(pkt.get(), c->time_base, st->time_base);
#ifdef VT_MEDIA_TEST_SEAMS
        if (settings.failAfterBytesForTest > 0 && bytesWritten + pkt->size > settings.failAfterBytesForTest) {
            av_packet_unref(pkt.get());
            fail("write failed: simulated disk full");
            return false;
        }
        bytesWritten += pkt->size;
#endif
        rc = av_interleaved_write_frame(out.get(), pkt.get());  // takes the packet's contents
        if (rc < 0) { fail("write failed: " + averr(rc)); return false; }
        if (video) {
            std::lock_guard lk(mu);
            ++status.framesWritten;
        }
    }
}

bool Recorder::Impl::encodeAudio(int samples) {
    AVFrame* f = aframe.get();
    f->nb_samples = samples;
    int rc = av_frame_make_writable(f);
    if (rc < 0) { fail("out of memory: " + averr(rc)); return false; }
    const float* src = pending.data() + pendingPos;
    if (actx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
        auto* l = reinterpret_cast<float*>(f->extended_data[0]);
        auto* r = reinterpret_cast<float*>(f->extended_data[1]);
        for (int i = 0; i < samples; ++i) { l[i] = src[2 * i]; r[i] = src[2 * i + 1]; }
    } else {
        auto* d = reinterpret_cast<int16_t*>(f->extended_data[0]);
        for (int i = 0; i < 2 * samples; ++i)
            d[i] = int16_t(std::lrint(std::clamp(src[i], -1.f, 1.f) * 32767.f));
    }
    f->pts = audioPts;
    f->duration = samples;
    audioPts += samples;
    pendingPos += size_t(samples) * 2;
    return send(actx.get(), f);
}

bool Recorder::Impl::encodeEntry(const Entry& e) {
    if (e.hasVideo) {
        AVFrame* f = vframe.get();
        int rc = av_frame_make_writable(f);
        if (rc < 0) { fail("out of memory: " + averr(rc)); return false; }
        if (rgb) {
            // RGBA -> bgr0: a byte swizzle, so the pixels stay exact.
            for (int y = 0; y < rect.h; ++y) {
                const uint8_t* s = e.px.data() + size_t(y) * size_t(rect.w) * 4;
                uint8_t* d = f->data[0] + ptrdiff_t(y) * f->linesize[0];
                for (int x = 0; x < rect.w; ++x, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 0; }
            }
        } else {
            srcView->data[0] = const_cast<uint8_t*>(e.px.data());
            srcView->extended_data = srcView->data;
            f->colorspace = AVCOL_SPC_SMPTE170M;
            f->color_range = AVCOL_RANGE_MPEG;
            rc = sws_scale_frame(sws.get(), f, srcView.get());
            if (rc < 0) { fail("colour conversion failed: " + averr(rc)); return false; }
        }
        // sws_scale_frame copies the source's properties over the destination's: set ours after.
        f->pts = e.pts;
        f->duration = 1;
        f->sample_aspect_ratio = vctx->sample_aspect_ratio;
        f->colorspace = vctx->colorspace;
        f->color_range = vctx->color_range;
        f->color_primaries = vctx->color_primaries;
        f->color_trc = vctx->color_trc;
        if (!send(vctx.get(), f)) return false;
    }
    if (pendingPos > 0 && pendingPos * 2 >= pending.size()) {  // keep the buffer small
        pending.erase(pending.begin(), pending.begin() + std::ptrdiff_t(pendingPos));
        pendingPos = 0;
    }
    pending.insert(pending.end(), e.audio.begin(), e.audio.end());
    while (pending.size() - pendingPos >= size_t(kAudioChunk) * 2)
        if (!encodeAudio(kAudioChunk)) return false;
    return true;
}

void Recorder::Impl::run() {
    for (;;) {
        std::unique_ptr<Entry> e;
        {
            std::unique_lock lk(mu);
            cvWork.wait(lk, [&] { return queue.empty() ? stopping : (!paused || stopping); });
            if (queue.empty()) break;
            e = std::move(queue.front());
            queue.pop_front();
            if (e->hasVideo) { --queuedVideo; cvSpace.notify_one(); }
        }
        if (!failed) encodeEntry(*e);
        std::lock_guard lk(mu);
        freeList.push_back(std::move(e));
    }
    finish();
}

void Recorder::Impl::finish() {
    if (!failed) {
        const size_t rest = (pending.size() - pendingPos) / 2;
        if (rest > 0) encodeAudio(int(rest));
        if (!failed) send(vctx.get(), nullptr);
        if (!failed) send(actx.get(), nullptr);
    }
    // A failed recording still gets its trailer, so what was written stays playable.
    if (headerWritten) {
        const int rc = av_write_trailer(out.get());
        if (rc < 0 && !failed) fail("cannot write the trailer: " + averr(rc));
    }
    if (out && out->pb && !(out->oformat->flags & AVFMT_NOFILE)) {
        const int rc = avio_closep(&out->pb);
        if (rc < 0 && !failed) fail("cannot close the file: " + averr(rc));
    }
    sws.reset(); vframe.reset(); srcView.reset(); aframe.reset(); pkt.reset();
    vctx.reset(); actx.reset(); out.reset();
    vst = ast = nullptr;
    pending.clear();
    pendingPos = 0;
}

Recorder::Recorder() : impl_(std::make_unique<Impl>()) {}
Recorder::~Recorder() { stop(); }

bool Recorder::start(const RecorderSettings& s, std::string* error) {
    Impl& m = *impl_;
    std::lock_guard serial(m.startMu);
    std::string err;
    bool failedRecording = false;
    {
        std::unique_lock lk(m.mu);
        m.cvStopped.wait(lk, [&] { return !m.joining; });
        // A recording whose encoder failed is over: finish it (trailer, close) and start anew.
        failedRecording = m.thread.joinable() && !m.status.error.empty();
        if (m.thread.joinable() && !failedRecording) err = "already recording";
    }
    if (failedRecording) stop();
    if (err.empty()) {
        m.headerWritten = m.failed = false;
        m.audioPts = 0;
        m.pending.clear();
        m.pendingPos = 0;
#ifdef VT_MEDIA_TEST_SEAMS
        m.bytesWritten = 0;
#endif
        if (!m.open(s, err)) {
            // Nothing is queued and no thread runs; drop what was half built.
            m.sws.reset(); m.vframe.reset(); m.srcView.reset(); m.aframe.reset(); m.pkt.reset();
            m.vctx.reset(); m.actx.reset(); m.out.reset();
        }
    }
    std::lock_guard lk(m.mu);
    if (!err.empty()) {
        if (error) *error = err;
        if (err != "already recording") m.status = RecorderStatus{false, err, 0, 0};
        return false;
    }
    m.queue.clear();
    m.queuedVideo = 0;
    m.accepting = true;
    m.stopping = false;
    m.haveFirst = false;
    m.status = RecorderStatus{true, {}, 0, 0};
    m.thread = std::thread([&m] { m.run(); });
    return true;
}

void Recorder::stop() {
    Impl& m = *impl_;
    std::thread encoder;
    {
        std::unique_lock lk(m.mu);
        m.cvStopped.wait(lk, [&] { return !m.joining; }); // another stop() is finishing it
        if (!m.thread.joinable()) return;
        m.accepting = false;
        m.stopping = true;
        m.joining = true;
        m.cvWork.notify_all();
        m.cvSpace.notify_all();
        encoder = std::move(m.thread);
    }
    encoder.join();
    std::lock_guard lk(m.mu);
    m.status.recording = false;
    m.joining = false;
    m.cvStopped.notify_all();
}

RecorderStatus Recorder::status() const {
    std::lock_guard lk(impl_->mu);
    return impl_->status;
}

void Recorder::onProgram(int64_t tick, const vt::Frame& program, std::span<const float> stereo) {
    impl_->onProgram(tick, program, stereo);
}

#ifdef VT_MEDIA_TEST_SEAMS
void Recorder::setEncoderPausedForTest(bool paused) {
    std::lock_guard lk(impl_->mu);
    impl_->paused = paused;
    impl_->cvWork.notify_all();
}
#endif

} // namespace vt::media
