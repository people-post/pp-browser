#pragma once

#include "common/Value.h"
#include "common/thread/ThreadChannel.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** 2: the sender and recipient devices, signed by the sending device (M021; sign v3). */
inline constexpr int kRelayEnvelopeVersion = 2;

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

/**
 * One device's copy of a message (M022): sealed in that device pair's session and numbered there — or, with
 * no recipient device, the account copy every device of the account reads (M021).
 */
struct RelayCopy {
  std::string recipient_device;
  uint32_t session_epoch = 1;
  uint64_t sender_seq = 0;
  std::optional<std::string> key_init_b64;
  std::string payload_b64;

  bool operator==(const RelayCopy&) const = default;
};

/** At most this many copies in one envelope: the account's devices (M021: 5) and some slack. */
inline constexpr size_t kMaxRelayCopies = 8;

struct RelayE2eBody {
  /** On the wire: the copies, one per recipient device (or the one account copy), signed together. */
  std::vector<RelayCopy> copies;
  /**
   * The sender's account card (JSON; docs/contracts/ACCOUNT_CARD.md), when a copy carries a key_init or is an
   * account copy: the account's device list, which lists the sending device — the receiver verifies the
   * envelope and answers every device with no lookup. Signed by the account it names, not by the envelope
   * signature.
   */
  std::optional<Object> sender_card;
  /**
   * The `issued_at` of the sender's newest account card (M021 d6), on every envelope: a receiver whose kept
   * card is older fetches the newer one from the sending device (a device taken out stops getting copies sooner).
   * Not signed — a false value costs one card fetch; the card proves itself.
   */
  std::optional<int64_t> sender_card_at;
  /** The copy this device reads (SelectRelayCopy): its payload and key_init. Not on the wire. */
  std::string payload_b64;
  std::optional<std::string> key_init_b64;
};

struct RelayMessageBody {
  RelayE2eBody e2e;
};

struct RelayEnvelope {
  int envelope_version = kRelayEnvelopeVersion;
  std::string message_id;
  std::string sender_relay_id;
  std::string sender_contact_id;
  /** The sending device's PeerId (M021): its key signs the envelope; numbering is per sending device. */
  std::string sender_device;
  /**
   * The copy this device reads (SelectRelayCopy; not on the wire): the device it is for — a copy in that
   * device pair's session — or empty for an account copy, in the account-level session (a first contact,
   * the control lane); with its numbering.
   */
  std::string recipient_device;
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
  /**
   * P012 — a hint to the relay, not part of the envelope (neither its wire form nor its
   * signature): wake the recipient as for an incoming call (a call invite).
   */
  bool relay_wake_urgent = false;
};

/**
 * The copies `envelope` carries: its copy list, or — none listed — its read view as its one copy (an
 * envelope built for one recipient).
 */
inline std::vector<RelayCopy> RelayCopiesOf(const RelayEnvelope& envelope) {
  if (!envelope.body.e2e.copies.empty()) {
    return envelope.body.e2e.copies;
  }
  if (envelope.body.e2e.payload_b64.empty()) {
    return {};
  }
  return {RelayCopy{.recipient_device = envelope.recipient_device,
                    .session_epoch = envelope.session_epoch,
                    .sender_seq = envelope.sender_seq,
                    .key_init_b64 = envelope.body.e2e.key_init_b64,
                    .payload_b64 = envelope.body.e2e.payload_b64}};
}

/** `envelope` as read in `copy` (one of its copies): its device, numbering, payload and key_init. */
inline RelayEnvelope ViewRelayCopy(RelayEnvelope envelope, const RelayCopy& copy) {
  envelope.recipient_device = copy.recipient_device;
  envelope.session_epoch = copy.session_epoch;
  envelope.sender_seq = copy.sender_seq;
  envelope.body.e2e.payload_b64 = copy.payload_b64;
  envelope.body.e2e.key_init_b64 = copy.key_init_b64;
  return envelope;
}

/**
 * The copy device `device` reads (M022): its own, else the account copy; none when the envelope has copies
 * for other devices only (a sibling's — or one missing for us).
 */
inline std::optional<RelayEnvelope> SelectRelayCopy(const RelayEnvelope& envelope, const std::string& device) {
  const std::vector<RelayCopy> copies = RelayCopiesOf(envelope);
  const RelayCopy* account_copy = nullptr;
  for (const RelayCopy& copy : copies) {
    if (!device.empty() && copy.recipient_device == device) {
      return ViewRelayCopy(envelope, copy);
    }
    if (copy.recipient_device.empty()) {
      account_copy = &copy;
    }
  }
  if (account_copy) {
    return ViewRelayCopy(envelope, *account_copy);
  }
  return std::nullopt;
}

/** An envelope with one copy, viewed as it (a control-lane message, a copy sent over a link). */
inline RelayEnvelope WithSingleRelayCopy(RelayEnvelope envelope, RelayCopy copy) {
  envelope.body.e2e.copies = {copy};
  return ViewRelayCopy(std::move(envelope), copy);
}

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
