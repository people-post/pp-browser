#pragma once

#include "common/chat/RelayEnvelope.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
#include <string>

namespace pbr {

/** rpc-kind live chat — separate OPEN from history (handler ownership). */
inline constexpr const char* kRpcChatProtocolId = "/pp-browser/rpc/chat/1.0.0";
/** rpc-kind peer history sync — separate OPEN from live chat. */
inline constexpr const char* kRpcHistoryProtocolId = "/pp-browser/rpc/history/1.0.0";
inline constexpr const char* kDirectChatProtocolId = kRpcChatProtocolId;
/** D060 peer-direct history — `/pp-browser/rpc/history/1.0.0` (rpc kind; own OPEN). */
inline constexpr const char* kChatHistoryProtocolId = kRpcHistoryProtocolId;

/** Direct push of RelayEnvelope over the peer mesh (Amp ChannelSession). */
class IDirectMessageClient {
public:
  using InboundHandler = std::function<void(RelayEnvelope envelope)>;

  virtual ~IDirectMessageClient() = default;
  virtual bool IsPeerReachable(const std::string& peer_identity_value) const = 0;
  virtual Roe<void> SendEnvelope(const std::string& peer_relay_user_id, const RelayEnvelope& envelope) = 0;
};

} // namespace pbr
