#include "feature/settings/OwnDevices.h"

#include "foundation/i18n/LocalizationService.h"

#include <utility>

namespace pbr {

namespace {

/** Enough of a PeerId to tell the account's few devices apart (its start is the same multihash prefix). */
constexpr size_t kShortIdChars = 6;

} // namespace

std::string OwnDeviceLabel(const std::string& peer_id, const bool is_this_device) {
  if (is_this_device) {
    return Tr("settings.security.devices.this_device");
  }
  const std::string short_id = peer_id.size() > kShortIdChars ? peer_id.substr(peer_id.size() - kShortIdChars)
                                                              : peer_id;
  return Tr("settings.security.devices.device", {{"id", short_id}});
}

OwnDevicesView OwnDevicesViewFor(const std::vector<std::string>& peer_ids, const std::string& self_peer_id,
                                 const bool signed_out) {
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
    device.label = OwnDeviceLabel(device.peer_id, device.is_this_device);
    view.devices.push_back(std::move(device));
  }
  return view;
}

} // namespace pbr
