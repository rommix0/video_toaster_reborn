#pragma once
// The capture devices DirectShow offers, for choosing an input's videoDevice / audioDevice.
#include <string>
#include <vector>

namespace vt::media {

struct DeviceInfo {
    std::string name; // friendly name, for people
    std::string id;   // DirectShow's alternative name: unique, and usable as InputConfig::videoDevice/audioDevice
};

enum class DeviceKind { Video, Audio };

// The DirectShow devices that offer `kind`. Empty when there are none or listing fails (the
// reason goes to *error, which is cleared on success); never throws.
std::vector<DeviceInfo> listDevices(DeviceKind, std::string* error = nullptr);

} // namespace vt::media
