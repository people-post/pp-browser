#pragma once

#include "common/ThreadChannel.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

inline constexpr int kRelayEnvelopeVersion = 1;

struct RelayRoute {
  std::string kind = "direct";
  ThreadChannel channel = ThreadChannel::E2e;
  std::optional<std::string> group_id;
};

struct RelayE2eBody {
  std::string payload_b64;
  std::optional<std::string> key_init_b64;
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
