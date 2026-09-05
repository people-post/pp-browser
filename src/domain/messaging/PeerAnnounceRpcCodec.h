#pragma once

#include "domain/messaging/PeerAnnounceTypes.h"

#include "common/chat/IDirectMessageClient.h"
#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "common/PbrCompat.h"

namespace pbr {

inline constexpr const char* kPeerAnnounceOpTipPush = "tip_push";
inline constexpr const char* kPeerAnnounceOpTipAck = "tip_ack";

struct PeerAnnounceTipPush {
  PeerAnnounceTip tip;
};

struct PeerAnnounceTipAck {
  bool ok = false;
  std::string error;
  uint64_t seq = 0;
  uint64_t epoch = 0;
};

using PeerAnnounceRpcMessage = std::variant<PeerAnnounceTipPush, PeerAnnounceTipAck>;

Roe<std::string> EncodePeerAnnounceTipPush(const PeerAnnounceTip& tip);
Roe<std::string> EncodePeerAnnounceTipAck(const PeerAnnounceTipAck& ack);
Roe<PeerAnnounceRpcMessage> DecodePeerAnnounceRpcJson(std::string_view json);

} // namespace pbr
