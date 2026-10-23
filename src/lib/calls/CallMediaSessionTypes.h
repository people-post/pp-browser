#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The vocabulary of a call's 1:1 media session (V033): its phases and events, the kind of link it
 * rides and what a connect names. The transport (feature/call_media) runs it; the pure decisions
 * over it (CallMediaSessionLogic, CallMediaBundleLogic) live beside it.
 */

/** Transport session phases for 1:1 call-media (V033). Product UX phases are the calls' projection (LiveCall::Phase). */
enum class CallMediaSessionPhase {
  Idle = 0,
  Dialing,
  HelloOutbound,
  HelloInbound,
  Adopting,
  MediaReady,
  Detaching,
};

enum class CallMediaSessionEvent {
  ConnectRequested = 0,
  OpenStreamOk,
  OpenStreamFail,
  InboundStream,
  HelloOk,
  HelloFail,
  AdoptWon,
  AdoptLost,
  DuplexStarted,
  DuplexEof,
  DuplexError,
  DetachRequested,
  ConnectTimeout,
  HandlerCleared,
  ConnectSuperseded,
};

struct CallMediaDirectConnectParams {
  std::string peer_key;
  std::string call_id;
  uint32_t media_epoch = 1;
  ByteVector media_key;
  bool offerer = true;
};

/** Kind of mesh link the active call-media channels are bound on (path label truth). */
enum class CallMediaLinkKind {
  Unknown,
  /** ADP association (dialed or punched). */
  Direct,
  /** Nested link over a relay circuit carrier. */
  Relayed,
};

} // namespace pbr
