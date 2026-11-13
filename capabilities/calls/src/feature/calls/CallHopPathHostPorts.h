#pragma once

#include "lib/calls/CallControlCodec.h"
#include "feature/calls/CallMedia.h"

#include "common/Error.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What a call's group path asks the device and the control lane it sends on (V046); what it tells the
 * device goes up as `HopReport` events (V057 2).
 */
struct CallHopPathHostPorts {
  std::function<Roe<std::string>()> local_account_identity;
  std::function<Roe<void>(const std::string& call_id, CallControlType type, const std::string& detail_json,
                          const std::string& display, const std::string& skip_identity)>
      fan_out_joined;
  std::function<Roe<void>(const std::string& peer_identity, CallControlType type, const std::string& detail_json,
                          const std::string& display)>
      send_direct;
  /** The call's `CallMedia` (engine + seat use); null for a call not admitted here. */
  std::function<CallMedia*(const std::string& call_id)> call_media;

  bool IsBound() const { return static_cast<bool>(local_account_identity); }
};

} // namespace pbr
