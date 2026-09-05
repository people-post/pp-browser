#pragma once

#include "common/chat/RelayEnvelope.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
#include <string>

namespace pbr {

inline constexpr const char* kRpcProtocolId = "/pp-browser/rpc/1.0.0";
inline constexpr const char* kDirectChatProtocolId = kRpcProtocolId;

/** Direct push of RelayEnvelope over the peer mesh (Amp ChannelSession). */
class IDirectMessageClient {
public:
  using InboundHandler = std::function<void(RelayEnvelope envelope)>;

  virtual ~IDirectMessageClient() = default;
  virtual bool IsPeerReachable(const std::string& peer_identity_value) const = 0;
  virtual Roe<void> SendEnvelope(const std::string& peer_relay_user_id, const RelayEnvelope& envelope) = 0;
};

} // namespace pbr
