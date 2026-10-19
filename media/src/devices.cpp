#include "vt/media/devices.h"

#include <algorithm>
#include <new>

#include "vt/media/ffmpeg.h"

namespace vt::media {

std::vector<DeviceInfo> listDevices(DeviceKind kind, std::string* error) {
    const auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return std::vector<DeviceInfo>{};
    };
    initMedia();
    const AVInputFormat* dshow = av_find_input_format("dshow");
    if (!dshow) return fail("dshow is not available");
    AVDeviceInfoList* list = nullptr;
    if (const int err = avdevice_list_input_sources(dshow, nullptr, nullptr, &list); err < 0) {
        avdevice_free_list_devices(&list);
        return fail(averr(err));
    }
    const AVMediaType want = kind == DeviceKind::Video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    std::vector<DeviceInfo> out;
    try {
        for (int i = 0; i < list->nb_devices; ++i) {
            const AVDeviceInfo& d = *list->devices[i];
            const AVMediaType* types = d.media_types;
            if (!d.device_name || !types || std::find(types, types + d.nb_media_types, want) == types + d.nb_media_types)
                continue;
            out.push_back({d.device_description ? d.device_description : d.device_name, d.device_name});
        }
    } catch (const std::bad_alloc&) {
        avdevice_free_list_devices(&list);
        return fail("out of memory");
    }
    avdevice_free_list_devices(&list);
    if (error) error->clear();
    return out;
}

} // namespace vt::media
