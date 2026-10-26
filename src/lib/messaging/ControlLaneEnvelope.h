#pragma once

#include "common/chat/RelayEnvelope.h"
#include "common/thread/ThreadRecordTypes.h"
#include "foundation/crypto/CryptoTypes.h"
#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Who sends to whom on the control lane, under which e2e_public session. */
struct ControlLaneSendParams {
  std::string sender_account_id;
  std::string sender_relay_id;
  std::string peer_account_id;
  /** Where the relay delivers: the peer's relay id, or its account id when only Amp reaches it. */
  std::string route_party;
  uint32_t session_epoch = 1;
  ByteVector master_psk;
  /** Set on the send that opens the session (AutoKey), for the peer to derive it. */
  std::optional<std::string> key_init_b64;
  /** This device's PeerId (M021): it signs the envelope. */
  std::string sender_device;
  /** Our account card (JSON; docs/contracts/ACCOUNT_CARD.md): every lane envelope carries it (an account copy). */
  std::optional<Object> sender_card;
  /** The relay index within the lane's stream (unique, rising per sender). */
  uint64_t order_key = 0;
};

/**
 * A signed control-lane envelope carrying `control` (an identified system message): route
 * kRelayRouteControl, unsequenced (seq 0), E2E under the pair's e2e_public session, its own relay
 * stream — an account copy (every device of the peer reads it; M021). `sign` signs the envelope's sign bytes
 * with this device's key.
 */
Roe<RelayEnvelope> BuildControlLaneEnvelope(const ControlLaneSendParams& params, const ThreadMessage& control,
                                            const std::function<Roe<std::string>(const std::vector<uint8_t>&)>& sign);

} // namespace pbr
