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

/**
 * One copy of a lane message: for one of the peer's devices, under that device pair's e2e_public session (M021,
 * the chat stream's), or — no device — the account copy, under the account pair's.
 */
struct ControlLaneCopy {
  /** The peer's device it is for; empty for the account copy (every device of the account reads it). */
  std::string recipient_device;
  uint32_t session_epoch = 1;
  ByteVector master_psk;
  /** Set while the copy's session is one we opened and the peer has not been heard under it, for it to derive. */
  std::optional<std::string> key_init_b64;
};

/** Who sends to whom on the control lane, and the copies (each under its pair's e2e_public session). */
struct ControlLaneSendParams {
  std::string sender_account_id;
  std::string sender_relay_id;
  std::string peer_account_id;
  /** Where the relay delivers: the peer's relay id, or its account id when only Amp reaches it. */
  std::string route_party;
  /** One per device of the peer's we know, or the one account copy (1 to kMaxRelayCopies). */
  std::vector<ControlLaneCopy> copies;
  /** This device's PeerId (M021): it signs the envelope. */
  std::string sender_device;
  /**
   * Our account card (JSON; docs/contracts/ACCOUNT_CARD.md), when this envelope carries it
   * (ControlLaneCarriesCard): about 13 KB plus 4 KB per device, so not on every lane message.
   */
  std::optional<Object> sender_card;
  /** Our newest card's `issued_at`, on every envelope (M021 d6): a peer holding an older one asks for it. */
  std::optional<int64_t> sender_card_at;
  /** The relay index within the lane's stream (unique, rising per sender). */
  uint64_t order_key = 0;
};

/**
 * M024 bridge, for one release: a lane message between two accounts that each have one device (as far as the
 * sender knows) goes as the account copy, as before M024 — an app before it refuses a lane envelope with device
 * copies, so a single-device peer still rings while it updates. Drop it (false, then the rule) in the release
 * after: every lane message to a peer with a known card goes per device.
 */
inline constexpr bool kControlLaneAccountCopyBetweenSingleDevices = true;

/**
 * Whether a lane message to an account goes as its one account copy (M024): when we hold no card for it
 * (`peer_devices` 0: a first contact), or — the bridge above — when it and we have one device each. Otherwise one
 * copy per device on the peer's newest card, each under that device pair's session.
 */
bool ControlLaneUsesAccountCopy(size_t peer_devices, size_t own_devices);

/**
 * The devices of the peer's a lane record addresses, for telling them our card (ControlLaneCarriesCard): each device
 * copy's device; for an account copy, every device on the peer's card we hold (`card_devices`) — or, holding none (a
 * first contact), the account itself (an empty id).
 */
std::vector<std::string> ControlLaneAddressedDevices(const std::vector<ControlLaneCopy>& copies,
                                                     const std::vector<std::string>& card_devices);

/**
 * Whether a lane envelope carries our account card: when it opens a session (`opens_session`), when the caller
 * says the peer may hold none of our cards (`asked`: a device list, an ask, a restored device's member check, a
 * ringing invite), or when any device it addresses (ControlLaneAddressedDevices) has not been sent the card at
 * `card_at` by this device yet (`told_at`, one per addressed device: the version last sent it, none). A device just
 * linked to the peer's account has been told nothing, though its account was: it checks the envelope with the card
 * it rides, and holds no other. Otherwise the peer checks the envelope with the card it keeps, and asks for a newer
 * one. One record carries the card once, whichever of its copies needed it.
 */
bool ControlLaneCarriesCard(bool opens_session, bool asked, const std::vector<std::optional<int64_t>>& told_at,
                            int64_t card_at);

/**
 * A signed control-lane envelope carrying `control` (an identified system message): route
 * kRelayRouteControl, unsequenced (seq 0), its own relay stream; one record with `params.copies` — the same
 * plaintext sealed in each copy's session (M024: a copy per device of the peer's, or the account copy). `sign`
 * signs the envelope's sign bytes with this device's key.
 */
Roe<RelayEnvelope> BuildControlLaneEnvelope(const ControlLaneSendParams& params, const ThreadMessage& control,
                                            const std::function<Roe<std::string>(const std::vector<uint8_t>&)>& sign);

} // namespace pbr
