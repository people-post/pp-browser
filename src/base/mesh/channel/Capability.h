#pragma once

#include "base/mesh/channel/ChannelWire.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <string>
#include <vector>

namespace pbr::amp {

/** Channel 0 capability payload (binary v1). */
struct CapabilityPayload {
  std::string local_peer_id;
  std::vector<std::string> listen_multiaddrs;
  std::vector<std::string> protocols;
};

class CapabilityCodec {
public:
  static Roe<std::vector<uint8_t>> Encode(const CapabilityPayload& payload);
  static Roe<CapabilityPayload> Decode(std::span<const uint8_t> wire);
};

} // namespace pbr::amp
