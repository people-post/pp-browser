#include "feature/settings/OwnDevices.h"

#include "common/DeviceName.h"
#include "foundation/i18n/LocalizationService.h"

#include <utility>

namespace pbr {

OwnDevicesView OwnDevicesViewFor(const std::vector<std::string>& peer_ids, const std::string& self_peer_id,
                                 const bool signed_out, const std::map<std::string, std::string>& names) {
  OwnDevicesView view;
  view.signed_out = signed_out;
  // No list yet: this device's first card will start one with itself first (the backend lets it add a device).
  view.this_is_main = !signed_out && (peer_ids.empty() || peer_ids.front() == self_peer_id);
  view.devices.reserve(peer_ids.size());
  for (size_t i = 0; i < peer_ids.size(); ++i) {
    OwnDeviceView device;
    device.peer_id = peer_ids[i];
    device.is_this_device = !self_peer_id.empty() && peer_ids[i] == self_peer_id;
    device.is_main = i == 0;
    device.can_manage = view.this_is_main && !device.is_this_device;
    const auto given = names.find(device.peer_id);
    device.name = given == names.end() ? std::string() : NormalizeDeviceName(given->second);
    device.named = !device.name.empty();
    if (!device.named) {
      device.name = Tr("settings.security.devices.unnamed");
    }
    view.devices.push_back(std::move(device));
  }
  return view;
}

} // namespace pbr
