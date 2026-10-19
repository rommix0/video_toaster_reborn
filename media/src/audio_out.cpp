#include "vt/media/audio_out.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/opt.h>
}

#include "vt/log.h"
#include "vt/media/ffmpeg.h"

namespace vt::media {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kRate = 48000;
constexpr double kKp = 0.015;      // DriftController: ratio change per unit of level error (relative to the target)
constexpr double kSmooth = 0.02;   // DriftController: weight of a new level reading in the smoothed level

// COM on the calling thread for as long as this lives. A thread that is already in the other
// apartment model (Qt's GUI thread is STA) keeps what it has: COM is usable and is not torn down.
struct ComScope {
    bool owned;
    ComScope() : owned(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() { if (owned) CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

std::string hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

std::string utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// An id of a device as people and settings files see it.
std::string deviceId(IMMDevice* dev) {
    LPWSTR id = nullptr;
    if (FAILED(dev->GetId(&id)) || !id) return {};
    std::string s = utf8(id);
    CoTaskMemFree(id);
    return s;
}

// What the device mixes in, as swresample describes it.
struct DeviceFormat {
    AVSampleFormat fmt = AV_SAMPLE_FMT_NONE;
    int rate = 0, channels = 0, bytesPerFrame = 0;
    AVChannelLayout layout{};
    DeviceFormat() = default;
    DeviceFormat(const DeviceFormat&) = delete;
    DeviceFormat& operator=(const DeviceFormat&) = delete;
    ~DeviceFormat() { av_channel_layout_uninit(&layout); }
};

// Fills `out` from a WAVEFORMATEX; the reason it cannot when it returns false.
bool describe(const WAVEFORMATEX& wf, DeviceFormat& out, std::string& why) {
    WORD tag = wf.wFormatTag;
    DWORD mask = 0;
    if (tag == WAVE_FORMAT_EXTENSIBLE && wf.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wf);
        tag = WORD(ext.SubFormat.Data1);   // the KSDATAFORMAT_SUBTYPE_* GUIDs carry the format tag first
        mask = ext.dwChannelMask;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && wf.wBitsPerSample == 32) out.fmt = AV_SAMPLE_FMT_FLT;
    else if (tag == WAVE_FORMAT_PCM && wf.wBitsPerSample == 16) out.fmt = AV_SAMPLE_FMT_S16;
    else if (tag == WAVE_FORMAT_PCM && wf.wBitsPerSample == 32) out.fmt = AV_SAMPLE_FMT_S32;
    else {
        why = "the device mix format (tag " + std::to_string(tag) + ", " + std::to_string(wf.wBitsPerSample) +
              " bit) is not supported";
        return false;
    }
    out.rate = int(wf.nSamplesPerSec);
    out.channels = int(wf.nChannels);
    out.bytesPerFrame = int(wf.nBlockAlign);
    if (out.rate <= 0 || out.channels < 1 || out.bytesPerFrame != out.channels * av_get_bytes_per_sample(out.fmt)) {
        why = "the device mix format is not usable";
        return false;
    }
    if (mask == 0 || std::popcount(mask) != out.channels ||
        av_channel_layout_from_mask(&out.layout, mask) < 0)
        av_channel_layout_default(&out.layout, out.channels);
    return true;
}

} // namespace

std::vector<OutputDevice> listOutputDevices(std::string* error) {
    const auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return std::vector<OutputDevice>{};
    };
    try {
        ComScope com;
        ComPtr<IMMDeviceEnumerator> en;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (FAILED(hr)) return fail("cannot create the device enumerator: " + hex(hr));
        std::string defaultId;
        if (ComPtr<IMMDevice> def; SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) defaultId = deviceId(def.Get());
        ComPtr<IMMDeviceCollection> all;
        hr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all);
        if (FAILED(hr)) return fail("cannot list the playback devices: " + hex(hr));
        UINT n = 0;
        all->GetCount(&n);
        std::vector<OutputDevice> out;
        for (UINT i = 0; i < n; ++i) {
            ComPtr<IMMDevice> dev;
            if (FAILED(all->Item(i, &dev))) continue;
            OutputDevice d;
            d.id = deviceId(dev.Get());
            if (d.id.empty()) continue;
            ComPtr<IPropertyStore> props;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) d.name = utf8(pv.pwszVal);
                PropVariantClear(&pv);
            }
            if (d.name.empty()) d.name = d.id;
            d.isDefault = d.id == defaultId;
            out.push_back(std::move(d));
        }
        if (error) error->clear();
        return out;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

DriftController::DriftController(int targetFrames) : target_(std::max(targetFrames, 1)) {}

double DriftController::ratio(int fifoFrames) {
    if (!seeded_) {
        level_ = fifoFrames;
        seeded_ = true;
    } else {
        level_ += kSmooth * (fifoFrames - level_);
    }
    const double correction = kKp * (level_ - target_) / target_;
    return 1.0 + std::clamp(correction, -kMaxDeviation, kMaxDeviation);
}

FifoPacer::FifoPacer(int targetFrames) : target_(std::max(targetFrames, 1)), drift_(target_) {}

FifoPacer::Step FifoPacer::step(int fifoFrames) {
    Step s;
    if (!primed_) {
        if (fifoFrames < target_) return s;
        primed_ = true;
    } else if (fifoFrames <= target_ + kTrimMargin) {
        s.play = true;
        s.ratio = drift_.ratio(fifoFrames);
        return s;
    }
    // Just primed, or far too full (after a stall the loop catches up in one burst): drop the
    // oldest audio down to the target, a slow ratio would take minutes to drain it.
    s.keepFrames = target_;
    drift_ = DriftController(target_);
    s.play = true;
    s.ratio = drift_.ratio(target_);
    return s;
}

AudioOutput::AudioOutput() : ring_(size_t(kRingFrames) * 2, 0.0f) {}

AudioOutput::~AudioOutput() { stop(); }

AudioOutput::Status AudioOutput::status() const {
    Status s;
    s.running = running_.load();
    s.underruns = underruns_.load();
    s.overruns = overruns_.load();
    s.driftPpm = driftPpm_.load();
    std::lock_guard lock(mu_);
    s.error = error_;
    return s;
}

void AudioOutput::setError(const std::string& why) {
    vt::logWarning("audio output: " + why);
    std::lock_guard lock(mu_);
    error_ = why;
}

void AudioOutput::onProgram(int64_t, const vt::Frame&, std::span<const float> stereo) {
    if (!running_.load(std::memory_order_acquire)) return;
    int64_t n = int64_t(stereo.size() / 2);
    if (n > kFifoFrames) {   // never in practice (a tick is 800 frames); keep the newest second
        stereo = stereo.last(size_t(kFifoFrames) * 2);
        n = kFifoFrames;
    }
    if (n == 0) return;
    const int64_t w = w_.load(std::memory_order_relaxed);
    if (w + n - r_.load(std::memory_order_acquire) > kFifoFrames) overruns_.fetch_add(1, std::memory_order_relaxed);
    const int64_t slot = w & (kRingFrames - 1), first = std::min(n, kRingFrames - slot);
    std::memcpy(ring_.data() + slot * 2, stereo.data(), size_t(first) * 2 * sizeof(float));
    std::memcpy(ring_.data(), stereo.data() + first * 2, size_t(n - first) * 2 * sizeof(float));
    w_.store(w + n, std::memory_order_release);   // the consumer trims what exceeds kFifoFrames
}

bool AudioOutput::start(const AudioOutputSettings& settings, std::string* error) {
    const auto fail = [&](std::string why) {
        setError(why);
        if (error) *error = std::move(why);
        return false;
    };
    if (running_.load()) {   // not a failure of the run in progress: status().error stays as it is
        if (error) *error = "already running";
        return false;
    }
    if (thread_.joinable()) thread_.join();   // a run that ended by itself (device lost)
    if (stopEvent_) CloseHandle(stopEvent_);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return fail("cannot create the stop event");
    {
        std::lock_guard lock(mu_);
        error_.clear();
    }
    underruns_ = 0;
    overruns_ = 0;
    driftPpm_ = 0;
    r_.store(w_.load());   // nothing is written while not running: the FIFO starts empty

    std::promise<std::string> ready;
    std::future<std::string> result = ready.get_future();
    thread_ = std::thread([this, settings, ready = std::move(ready)]() mutable {
        try {
            renderMain(settings, ready);
        } catch (...) {   // out of memory: the device is released by the unwinding
            running_ = false;
            setError("playback failed unexpectedly");
            try { ready.set_value("playback failed unexpectedly"); } catch (const std::future_error&) {}
        }
    });
    if (const std::string why = result.get(); !why.empty()) {
        thread_.join();
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
        return fail(why);
    }
    if (error) error->clear();
    return true;
}

void AudioOutput::stop() {
    if (!thread_.joinable()) return;
    SetEvent(stopEvent_);
    thread_.join();
    CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
    running_ = false;
}

// The render thread: opens the device (the outcome, "" for success, goes to `ready`), then plays
// until stopped or the device is lost.
void AudioOutput::renderMain(const AudioOutputSettings& settings, std::promise<std::string>& ready) {
    bool signalled = false;
    const auto fail = [&](std::string why) {   // before the stream runs: start() reports it
        if (!signalled) ready.set_value(std::move(why));
        signalled = true;
    };
    ComScope com;
    DWORD mmcssTask = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTask);   // optional: nullptr is fine
    struct MmcssGuard { HANDLE h; ~MmcssGuard() { if (h) AvRevertMmThreadCharacteristics(h); } } mmcssGuard{mmcss};

    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) return fail("cannot create the device enumerator: " + hex(hr));
    if (settings.deviceId.empty()) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    else hr = en->GetDevice(wide(settings.deviceId).c_str(), &dev);
    if (FAILED(hr)) return fail((settings.deviceId.empty() ? "no default playback device: " : "playback device not found: ") + hex(hr));
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return fail("cannot open the playback device: " + hex(hr));

    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) return fail("cannot read the device mix format: " + hex(hr));
    DeviceFormat fmt;
    std::string why;
    const bool described = describe(*mix, fmt, why);
    if (described) {
        const REFERENCE_TIME buffer = REFERENCE_TIME(std::max(settings.bufferMs, 10)) * 10000;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, buffer, 0, mix, nullptr);
    }
    CoTaskMemFree(mix);
    if (!described) return fail(why);
    if (FAILED(hr)) return fail("cannot start the playback stream: " + hex(hr));

    UINT32 bufferFrames = 0;
    if (FAILED(hr = client->GetBufferSize(&bufferFrames))) return fail("cannot read the device buffer size: " + hex(hr));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return fail("cannot create the render event");
    struct EventGuard { HANDLE h; ~EventGuard() { CloseHandle(h); } } eventGuard{event};
    if (FAILED(hr = client->SetEventHandle(event))) return fail("cannot set the render event: " + hex(hr));
    if (FAILED(hr = client->GetService(IID_PPV_ARGS(&render)))) return fail("cannot get the render client: " + hex(hr));

    // 48 kHz stereo float in, the device's format out. The resampler is forced on (even at 48 kHz)
    // so swr_set_compensation has something to compensate with.
    SwrContext* rawSwr = nullptr;
    const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    int err = swr_alloc_set_opts2(&rawSwr, &fmt.layout, fmt.fmt, fmt.rate, &stereo, AV_SAMPLE_FMT_FLT, kRate, 0, nullptr);
    SwrPtr swr(rawSwr);
    if (err >= 0) err = av_opt_set_int(swr.get(), "swr_flags", SWR_FLAG_RESAMPLE, 0);
    if (err >= 0) err = swr_init(swr.get());
    if (err < 0) return fail("cannot convert to the device format: " + averr(err));

    const int targetFrames = std::max(1700, 24 * settings.bufferMs);   // the FIFO level to hold, in 48 kHz frames
    FifoPacer pacer(targetFrames);
    std::vector<float> in(size_t(std::ceil(double(bufferFrames) * kRate / fmt.rate * 1.01) + 256) * 2);

    BYTE* silence = nullptr;
    const auto level = [&] { return int(std::min(w_.load(std::memory_order_acquire) - r_.load(std::memory_order_relaxed), kFifoFrames)); };
    // Up to `frames` frames from the FIFO into `dst`, silence for what is missing; the number that was real.
    const auto pop = [&](float* dst, int frames) {
        const int64_t w = w_.load(std::memory_order_acquire);
        int64_t r = r_.load(std::memory_order_relaxed);
        r = std::max(r, w - kFifoFrames);                 // a full FIFO loses its oldest audio
        const int n = int(std::min<int64_t>(frames, w - r));
        const int64_t slot = r & (kRingFrames - 1), first = std::min<int64_t>(n, kRingFrames - slot);
        std::memcpy(dst, ring_.data() + slot * 2, size_t(first) * 2 * sizeof(float));
        std::memcpy(dst + first * 2, ring_.data(), size_t(n - first) * 2 * sizeof(float));
        std::fill(dst + n * 2, dst + frames * 2, 0.0f);
        r_.store(r + n, std::memory_order_release);
        return n;
    };

    // The device starts on a buffer of silence, so that what it asks for next is a period, not the whole buffer.
    if (SUCCEEDED(hr = render->GetBuffer(bufferFrames, &silence))) hr = render->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) return fail("cannot prepare the device buffer: " + hex(hr));
    if (FAILED(hr = client->Start())) return fail("cannot start the playback stream: " + hex(hr));
    running_.store(true, std::memory_order_release);
    fail("");   // started

    const HANDLE waits[2] = {stopEvent_, event};
    double owed = 0;       // input frames due to the resampler for the output made so far
    for (;;) {
        const DWORD got = WaitForMultipleObjects(2, waits, FALSE, 1000);
        if (got == WAIT_OBJECT_0) break;
        if (got == WAIT_FAILED) { hr = E_FAIL; break; }
        // A timeout is no event for a second: ask the device whether it is still there.
        UINT32 padding = 0;
        if (FAILED(hr = client->GetCurrentPadding(&padding))) break;
        if (got != WAIT_OBJECT_0 + 1 || padding >= bufferFrames) continue;
        const UINT32 frames = bufferFrames - padding;

        BYTE* data = nullptr;
        if (FAILED(hr = render->GetBuffer(frames, &data))) break;
        DWORD flags = 0;
        const FifoPacer::Step step = pacer.step(level());
        if (step.keepFrames >= 0) {   // the consumer moves r_ (the producer never does)
            const int64_t w = w_.load(std::memory_order_acquire);
            if (w - r_.load(std::memory_order_relaxed) > step.keepFrames) r_.store(w - step.keepFrames, std::memory_order_release);
            owed = 0;
        }
        if (!step.play) {
            std::memset(data, 0, size_t(frames) * size_t(fmt.bytesPerFrame));
            flags = AUDCLNT_BUFFERFLAGS_SILENT;
        } else {
            const double ratio = step.ratio;
            driftPpm_.store((ratio - 1.0) * 1e6, std::memory_order_relaxed);
            // swr_set_compensation(delta, distance): delta < 0 makes `distance` output frames use
            // -delta more input frames, i.e. the FIFO drains faster (ratio > 1).
            const int delta = int(std::lround((1.0 - ratio) * fmt.rate));
            swr_set_compensation(swr.get(), delta, fmt.rate);
            // Input frames the resampler uses per output frame, compensation included. Feeding
            // exactly that (`owed` carries the fraction) keeps what the resampler holds constant.
            const double inPerOut = double(kRate) / fmt.rate * (1.0 - double(delta) / fmt.rate);
            owed += frames * inPerOut;
            uint8_t* dst = data;
            int produced = 0;
            bool dry = false;
            for (int pass = 0; produced < int(frames) && pass < 4; ++pass) {
                const int need = int(frames) - produced;
                // A later pass is for output the resampler could not yet make from what it had.
                const int count = std::clamp(pass == 0 ? int(owed) : int(std::ceil(need * inPerOut)), pass == 0 ? 0 : 1, int(in.size() / 2));
                owed -= count;
                dry = pop(in.data(), count) < count || dry;
                const uint8_t* src = reinterpret_cast<const uint8_t*>(in.data());
                const int made = swr_convert(swr.get(), &dst, need, &src, count);
                if (made < 0) break;
                dst += size_t(made) * size_t(fmt.bytesPerFrame);
                produced += made;
            }
            if (dry) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
                pacer.ranDry();
                owed = 0;
            }
            if (produced < int(frames))
                std::memset(data + size_t(produced) * size_t(fmt.bytesPerFrame), 0, size_t(int(frames) - produced) * size_t(fmt.bytesPerFrame));
        }
        if (FAILED(hr = render->ReleaseBuffer(frames, flags))) break;
    }

    if (FAILED(hr)) setError(hr == AUDCLNT_E_DEVICE_INVALIDATED ? "the playback device was removed or changed (" + hex(hr) + ")"
                                                                  : "playback failed: " + hex(hr));
    running_.store(false, std::memory_order_release);   // after the error, so a caller that sees "stopped" sees why
    client->Stop();
}

} // namespace vt::media
