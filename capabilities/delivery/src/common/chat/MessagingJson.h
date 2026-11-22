#pragma once

#include "common/thread/ThreadTypes.h"
#include "common/Error.h"
#include "common/Value.h"

#include <string>
#include <vector>

namespace pbr {

std::string ThreadKindToString(ThreadKind kind);
ThreadKind ThreadKindFromString(const std::string& value);

std::string MessageDeliveryToString(MessageDelivery delivery);
MessageDelivery MessageDeliveryFromString(const std::string& value);

std::string MessageTransportToString(MessageTransport transport);
MessageTransport MessageTransportFromString(const std::string& value);

pp::common::Object ThreadToJson(const Thread& thread);
Thread ThreadFromJson(const pp::common::Object& json);

pp::common::Object ThreadMessageToJson(const ThreadMessage& message);
ThreadMessage ThreadMessageFromJson(const pp::common::Object& json);

pp::common::Object RelayEnvelopeToJson(const RelayEnvelope& envelope);
/** Application envelope JSON without relay routing fields (for blob encoding). */
pp::common::Object RelayEnvelopeToApplicationJson(const RelayEnvelope& envelope);
/**
 * Strict ingest, steps 1–2 of the receiver's order (release-compatibility SIGNED_FORMATS § 1.1, § 2.1): the
 * version (missing or below 4: an older app's, refused), then the fields and the extension area (`ext_b64`, kept as
 * received; malformed: refused). A higher `envelope_version` is a newer app's: read loosely, its routing and numbering
 * only, with `envelope_version` as sent, `needs_update` saying why and no payloads (INVENTORY T2) — the receive
 * pipeline closes its seqs and shows "update to view"; nothing in it is decrypted or verified. The typed blobs'
 * algorithm ids, the signature and the area's critical entries are the receiver's (EnvelopeSigner::UnknownAlgorithm
 * and Receive, with the sender's card).
 */
pp::Roe<RelayEnvelope> ParseRelayEnvelope(const pp::common::Object& json);

/** The send body without its `grant` (grant v2's JSON is lib/delivery's `RelayGrantToJson`; HttpRelayClient adds it). */
pp::common::Object RelayWireSendRecordToJson(const RelayWireSendRecord& record);
pp::Roe<RelayWireSendRecord> RelayWireSendRecordFromEnvelope(const RelayEnvelope& envelope);
pp::Roe<RelayEnvelope> RelayEnvelopeFromInboundRecord(const RelayInboundRecord& record);
pp::Roe<RelayInboundRecord> ParseRelayInboundRecord(const pp::common::Object& json);

pp::common::Object ChatHistoryRequestToJson(const ChatHistoryRequest& request);
pp::Roe<ChatHistoryRequest> ChatHistoryRequestFromJson(const pp::common::Object& json);
/** HTTP query string for GET /v1/streams/messages (generic relay). */
std::string StreamHistoryRequestToQueryString(const ChatHistoryRequest& request);
pp::common::Object ChatHistoryRequestToStreamHistoryJson(const ChatHistoryRequest& request);
pp::common::Object ChatHistoryResponseToJson(const ChatHistoryResponse& response);
pp::Roe<ChatHistoryResponse> ChatHistoryResponseFromJson(const pp::common::Object& json);

std::string ChatBlobOpToString(ChatBlobOp op);
ChatBlobOp ChatBlobOpFromString(const std::string& value);
pp::common::Object ChatBlobRequestToJson(const ChatBlobRequest& request);
pp::Roe<ChatBlobRequest> ChatBlobRequestFromJson(const pp::common::Object& json);
/** Short ack / error for chat-blob control responses. */
pp::common::Object ChatBlobAckToJson(bool ok, const std::string& error = {});

/**
 * The code a peer-direct server (history, blob, account card) answers a request it cannot read with: a newer
 * `auth_version`, an algorithm id or a critical extension it does not know (PeerRequestAuth v2, release-compatibility
 * SIGNED_FORMATS § 2.7) — never a silent close.
 */
inline constexpr char kUpdateNeededCode[] = "update_needed";
/** `{"ok":false,"code":"update_needed"}`. */
pp::common::Object UpdateNeededRefusalToJson();
/** True when `answer` is that refusal: the peer's app cannot read what we sent (it is older). */
bool IsUpdateNeededRefusal(const pp::common::Object& answer);

} // namespace pbr
