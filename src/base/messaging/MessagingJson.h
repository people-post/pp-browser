#pragma once

#include "base/people/ContactJson.h"
#include "base/people/ContactTypes.h"
#include "base/messaging/ThreadTypes.h"
#include "common/Error.h"
#include "common/Value.h"

#include <string>
#include <vector>

namespace pbr {

std::string ThreadKindToString(ThreadKind kind);
ThreadKind ThreadKindFromString(const std::string& value);

std::string ThreadChannelToString(ThreadChannel channel);
ThreadChannel ThreadChannelFromString(const std::string& value);

std::string MessageDeliveryToString(MessageDelivery delivery);
MessageDelivery MessageDeliveryFromString(const std::string& value);

std::string MessageTransportToString(MessageTransport transport);
MessageTransport MessageTransportFromString(const std::string& value);
/** post-v6d — short label for per-message transport badge. */
std::string MessageTransportBadgeLabel(MessageTransport transport);

pp::common::Object ThreadToJson(const Thread& thread);
Thread ThreadFromJson(const pp::common::Object& json);

pp::common::Object ThreadMessageToJson(const ThreadMessage& message);
ThreadMessage ThreadMessageFromJson(const pp::common::Object& json);

pp::common::Object RelayEnvelopeToJson(const RelayEnvelope& envelope);
/** Application envelope JSON without relay routing fields (for blob encoding). */
pp::common::Object RelayEnvelopeToApplicationJson(const RelayEnvelope& envelope);
/** Strict ingest — rejects legacy thread_id / flat body.text (D063). */
pp::Roe<RelayEnvelope> ParseRelayEnvelope(const pp::common::Object& json);

pp::common::Object RelayWireSendRecordToJson(const RelayWireSendRecord& record);
pp::Roe<RelayWireSendRecord> RelayWireSendRecordFromEnvelope(
    const RelayEnvelope& envelope);
pp::Roe<RelayEnvelope> RelayEnvelopeFromInboundRecord(const RelayInboundRecord& record);
pp::Roe<RelayInboundRecord> ParseRelayInboundRecord(const pp::common::Object& json);

pp::common::Object ChatHistoryRequestToJson(const ChatHistoryRequest& request);
pp::Roe<ChatHistoryRequest> ChatHistoryRequestFromJson(const pp::common::Object& json);
/** HTTP query string for GET /v1/streams/messages (generic relay). */
std::string StreamHistoryRequestToQueryString(const ChatHistoryRequest& request);
/** @deprecated Use StreamHistoryRequestToQueryString */
std::string ChatHistoryRequestToQueryString(const ChatHistoryRequest& request);
pp::common::Object ChatHistoryRequestToStreamHistoryJson(const ChatHistoryRequest& request);
pp::common::Object ChatHistoryResponseToJson(const ChatHistoryResponse& response);
pp::Roe<ChatHistoryResponse> ChatHistoryResponseFromJson(const pp::common::Object& json);

std::string ChatBlobOpToString(ChatBlobOp op);
ChatBlobOp ChatBlobOpFromString(const std::string& value);
pp::common::Object ChatBlobRequestToJson(const ChatBlobRequest& request);
pp::Roe<ChatBlobRequest> ChatBlobRequestFromJson(const pp::common::Object& json);
/** Short ack / error for chat-blob control responses. */
pp::common::Object ChatBlobAckToJson(bool ok, const std::string& error = {});

} // namespace pbr
