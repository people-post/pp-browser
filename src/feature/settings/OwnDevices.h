#pragma once

#include "feature/settings/SettingsPortsViews.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Me → Security → Your devices (M021 d4): the account's device list as the settings screen shows it.
 * `peer_ids` in list order — the first is the main device, the only one that adds or removes devices; it may
 * take another device off the account or make it the main one. A signed-out device manages nothing.
 */
OwnDevicesView OwnDevicesViewFor(const std::vector<std::string>& peer_ids, const std::string& self_peer_id,
                                 bool signed_out);

/** "This device", or "Device " and the last characters of its PeerId (devices have no names). */
std::string OwnDeviceLabel(const std::string& peer_id, bool is_this_device);

} // namespace pbr
