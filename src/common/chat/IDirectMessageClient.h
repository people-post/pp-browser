#pragma once

#include "common/chat/RelayEnvelope.h"
#include "common/CodedFailure.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include "common/ProtocolIds.h"

#include <cstdint>
#include <functional>
#include <string>

namespace pbr {

// The wire ids (kRpcChatProtocolId, kRpcHistoryProtocolId, …) are common/ProtocolIds.h's.
inline constexpr const char* kDirectChatProtocolId = kRpcChatProtocolId;
/** D060 peer-direct history — `/pp/rpc/history/1.0.0` (rpc kind; own OPEN). */
inline constexpr const char* kChatHistoryProtocolId = kRpcHistoryProtocolId;

/** Why a direct send did not go through (docs/contracts/CODED_FAILURE.md). */
enum class DirectSendErr : int32_t {
  Ok = 0,
  /** Direct messaging (the mesh) is not running. */
  NotStarted,
  /** No usable address for the peer. */
  NoAddress,
  /** The last dial to the peer failed recently. */
  DialInBackoff,
  /** Too many dials or links. */
  DialBusy,
  /** The peer did not answer a dial. */
  NoAnswer,
  /** The handshake failed. */
  Refused,
  /** The OS could not send. */
  TransportFailed,
  /** Reached the peer, but the chat channel did not open. */
  ChannelFailed,
  /** Sent, but the peer did not acknowledge it in time. */
  NotConfirmed,
  Generic,
};
using DirectSendFailure = CodedFailure<DirectSendErr>;
using DirectSendRoe = CodedRoe<void, DirectSendErr>;

/** Direct push of RelayEnvelope over the peer mesh (Amp ChannelSession). */
class IDirectMessageClient {
public:
  using InboundHandler = std::function<void(RelayEnvelope envelope)>;

  virtual ~IDirectMessageClient() = default;
  virtual bool IsPeerReachable(const std::string& peer_identity_value) const = 0;
  /**
   * A link to `peer_key` is up now (M022: that device gets its copy over it, the others share the relay);
   * an account: to any of its devices.
   */
  virtual bool IsPeerConnected(const std::string& peer_key) const { return false; }
  virtual DirectSendRoe SendEnvelope(const std::string& peer_relay_user_id, const RelayEnvelope& envelope) = 0;

  /** Prefer over SendEnvelope on product paths; default wraps sync SendEnvelope. */
  virtual void SendEnvelopeAsync(const std::string& peer_relay_user_id, const RelayEnvelope& envelope,
                                 std::function<void(DirectSendRoe)> on_done) {
    if (on_done) {
      on_done(SendEnvelope(peer_relay_user_id, envelope));
    }
  }
};

} // namespace pbr
