#pragma once

#include "common/directory/KeyCard.h"
#include "common/thread/ThreadChannel.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

inline constexpr int kRelayEnvelopeVersion = 1;

/** Route kinds: a 1:1 chat stream, a group stream, the control lane (signaling off the stream). */
inline constexpr const char* kRelayRouteDirect = "direct";
inline constexpr const char* kRelayRouteGroup = "group";
/**
 * The control lane: system-control messages a feature sends a peer outside the ordered chat stream
 * (call signaling) — E2E under the e2e_public session, not sequenced, never a transcript row.
 */
inline constexpr const char* kRelayRouteControl = "control";

struct RelayRoute {
  std::string kind = "direct";
  ThreadChannel channel = ThreadChannel::E2e;
  std::optional<std::string> group_id;
};

struct RelayE2eBody {
  std::string payload_b64;
  std::optional<std::string> key_init_b64;
  /**
   * The sender's key card, with a key_init only (docs/contracts/ACCOUNT_CARD.md): lets the receiver verify
   * the envelope and answer with no lookup. Signed by the account it names, not by the envelope signature.
   */
  std::optional<KeyCard> sender_keys;
  /** D095 — one AEAD blob per group member (recipient identity → base64). */
  std::optional<std::map<std::string, std::string>> member_payloads;
};

struct RelayMessageBody {
  RelayE2eBody e2e;
};

struct RelayEnvelope {
  int envelope_version = kRelayEnvelopeVersion;
  std::string message_id;
  std::string sender_relay_id;
  std::string sender_contact_id;
  RelayRoute route;
  RelayMessageBody body;
  uint64_t sender_seq = 0;
  uint32_t session_epoch = 1;
  /**
   * Group envelopes only (signed, sign v3): the epoch of the pairwise e2e_public session that keyed this
   * member's ciphertext. `session_epoch` stays the group's own numbering epoch — the two move apart
   * (a member removal moves the group's, a 1:1 restart the pair's), so neither is read as the other
   * (G011).
   */
  std::optional<uint32_t> key_epoch;
  int64_t timestamp = 0;
  std::string signature;
  /** Opaque conversation scope for relay indexing (unsigned). */
  std::string stream_key;
  /** Per-sender monotonic key for relay range fetch (= sender_seq in v1). */
  uint64_t order_key = 0;
  /** Delivery target — relay routing only (unsigned). */
  std::optional<std::string> recipient_contact_id;
  /** Unix ms relay store time from inbox poll (unsigned transport metadata). */
  std::optional<int64_t> relay_created_at_ms;
  /** Unix ms relay clock from the poll response that delivered this envelope. */
  std::optional<int64_t> relay_server_time_ms;
  /**
   * P012 — a hint to the relay, not part of the envelope (neither its wire form nor its
   * signature): wake the recipient as for an incoming call (a call invite).
   */
  bool relay_wake_urgent = false;
};

/** Opaque relay HTTP wire record for send (routing + blob). */
struct RelayWireSendRecord {
  std::string sender_contact_id;
  std::string recipient_contact_id;
  std::string stream_id;
  uint64_t index_key = 0;
  std::string blob_b64;
  int64_t timestamp = 0;
  std::string signature;
  /** P012 — wake the recipient as for an incoming call (unsigned hint; older relays ignore it). */
  bool urgent = false;
};

/** Opaque relay HTTP wire record returned on poll/history. */
struct RelayInboundRecord {
  std::string sender_contact_id;
  std::string stream_id;
  uint64_t index_key = 0;
  std::string blob_b64;
  /** Unix ms when the relay stored this row (unsigned transport metadata). */
  std::optional<int64_t> created_at_ms;
};

} // namespace pbr
