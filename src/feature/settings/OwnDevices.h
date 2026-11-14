#pragma once

#include "feature/settings/SettingsPortsViews.h"

#include <map>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Me → Security → Your devices (M021 d4): the account's device list as the settings screen shows it.
 * `peer_ids` in list order — the first is the main device, the only one that adds or removes devices; it may
 * take another device off the account, make it the main one or rename it. A signed-out device manages nothing.
 * `names` (peer_id → name, the account locker's table): shown for the devices on the list only, through
 * NormalizeDeviceName again; a device without one is "Unnamed". `own_system_name` (the cached system name, display
 * only, nothing is written): shown for this device while `names` has no entry for it.
 */
OwnDevicesView OwnDevicesViewFor(const std::vector<std::string>& peer_ids, const std::string& self_peer_id,
                                 bool signed_out, const std::map<std::string, std::string>& names = {},
                                 const std::string& own_system_name = {});

} // namespace pbr
