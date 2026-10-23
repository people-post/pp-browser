#pragma once

#include "lib/messaging/SendRelayOptions.h"

#include "common/Error.h"
#include "common/chat/RelayEnvelope.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/IPskSessionStore.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * What a send of ours writes and puts on the wire: the outbound message row, its sealed payload and
 * its envelope (unsigned — signing is the identity's). MeshDeliveryOrchestrator picks the
 * transports and runs the send; these hold the rules both the direct and the group send share.
 */

// --- The message row --------------------------------------------------------------------------------

/** A pending, relay-visible message from us (rich payload options applied). */
ThreadMessage NewOutboundMessage(const std::string& thread_id, const std::string& text,
                                 const SendRelayOptions& options, std::string id, int64_t now_ms);

/** The per-target reaction cap; never stops a delete. */
Roe<void> CheckAnnotationCap(const IThreadStore& store, const std::string& thread_id, const ThreadMessage& message);

/** Number `message` on its thread (sender seq, session epoch), check the reaction cap and append it. */
Roe<ThreadMessage> AppendOutboundMessage(IThreadStore& store, ThreadMessage message);

// --- A direct send ------------------------------------------------------------------------------------

struct DirectSendGate {
  bool direct_thread = false;
  ThreadChannel channel = ThreadChannel::E2ePublic;
  bool peer_known = false;
  /** E2E: the thread's integrity is broken (messaging paused). */
  bool compromised = false;
  /** E2E: the key is verified with the contact. */
  bool psk_ready = true;
  /** Public: this chat continues on another device. */
  bool locked_out = false;
};

/** Whether this device may send on the thread now (and why not). */
Roe<void> CheckDirectSendAllowed(const DirectSendGate& gate);

/** The peer's account KEM public key (the directory's, M015). */
using PeerKemPublicFn = std::function<Roe<ByteVector>(const ChatTargetKey&)>;

struct SealedPayload {
  std::string payload_b64;
  std::optional<std::string> key_init_b64;
};

/**
 * The direct send's payload: E2E channels encrypt under the epoch's master PSK — on a public chat
 * with none yet, under a session opened to the peer's account key, announced by a key_init on every
 * send until the peer is heard under it; plaintext channels encode. `options.key_init_b64` (a key
 * rotation) requires the current key and is carried as given.
 */
Roe<SealedPayload> SealDirectPayload(const ThreadMessage& message, const Thread& thread,
                                     const std::string& sender_account_id, const SendRelayOptions& options,
                                     IPskSessionStore& psk_store, const PeerKemPublicFn& peer_kem);

/** Who sends, as an envelope names us. */
struct EnvelopeSender {
  std::string relay_user_id;
  std::string account_id;
  /** Our key card, carried with every key_init so the peer can verify and answer with no lookup. */
  std::optional<KeyCard> keys;
};

/** A direct send's envelope to `route_party` (relay id, or the account for an Amp-only peer). */
RelayEnvelope BuildDirectEnvelope(const ThreadMessage& message, const EnvelopeSender& sender, ThreadChannel channel,
                                  const std::string& route_party, SealedPayload sealed);

/**
 * Call control prefers the relay when a route is known (Amp's open + ack per message races the
 * media setup); everything else tries Amp first when the peer is reachable there.
 */
bool DirectSendTriesAmpFirst(bool amp_reachable, bool has_relay_route, bool critical_lane);

// --- A group send -------------------------------------------------------------------------------------

/** One member's copy of a group send (its pairwise payload, and key_init when it has no session yet). */
RelayEnvelope BuildGroupMemberEnvelope(const ThreadMessage& message, const EnvelopeSender& sender,
                                       const std::string& group_id, const std::string& recipient,
                                       std::string payload_b64, std::optional<std::string> key_init_b64);

// --- The outbox: sends queued after a failed relay attempt ----------------------------------------

/** Control-lane signaling (call control and the like) is not worth sending once this old. */
inline constexpr int64_t kControlRetryWindowMs = 2 * 60 * 1000;

/**
 * Whether a queued send is still worth trying: control-lane signaling only within its window, and
 * nothing on a chat whose messaging is paused (integrity broken).
 */
bool OutboxSendStillWanted(bool control_lane, int64_t age_ms, bool thread_paused);

/** After another failed relay attempt (`attempts` so far): queue it again, or give up (failed). */
bool OutboxRetryAgain(int attempts);

} // namespace pbr
