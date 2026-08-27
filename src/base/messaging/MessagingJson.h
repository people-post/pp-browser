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

Object ThreadToJson(const Thread& thread);
Thread ThreadFromJson(const Object& json);

Object ThreadMessageToJson(const ThreadMessage& message);
ThreadMessage ThreadMessageFromJson(const Object& json);

Object RelayEnvelopeToJson(const RelayEnvelope& envelope);
/** Application envelope JSON without relay routing fields (for blob encoding). */
Object RelayEnvelopeToApplicationJson(const RelayEnvelope& envelope);
/** Strict ingest — rejects legacy thread_id / flat body.text (D063). */
Roe<RelayEnvelope> ParseRelayEnvelope(const Object& json);

Object RelayWireSendRecordToJson(const RelayWireSendRecord& record);
Roe<RelayWireSendRecord> RelayWireSendRecordFromEnvelope(const RelayEnvelope& envelope);
Roe<RelayEnvelope> RelayEnvelopeFromInboundRecord(const RelayInboundRecord& record);
Roe<RelayInboundRecord> ParseRelayInboundRecord(const Object& json);

Object ChatHistoryRequestToJson(const ChatHistoryRequest& request);
Roe<ChatHistoryRequest> ChatHistoryRequestFromJson(const Object& json);
/** HTTP query string for GET /v1/streams/messages (generic relay). */
std::string StreamHistoryRequestToQueryString(const ChatHistoryRequest& request);
/** @deprecated Use StreamHistoryRequestToQueryString */
std::string ChatHistoryRequestToQueryString(const ChatHistoryRequest& request);
Object ChatHistoryRequestToStreamHistoryJson(const ChatHistoryRequest& request);
Object ChatHistoryResponseToJson(const ChatHistoryResponse& response);
Roe<ChatHistoryResponse> ChatHistoryResponseFromJson(const Object& json);

std::string ChatBlobOpToString(ChatBlobOp op);
ChatBlobOp ChatBlobOpFromString(const std::string& value);
Object ChatBlobRequestToJson(const ChatBlobRequest& request);
Roe<ChatBlobRequest> ChatBlobRequestFromJson(const Object& json);
/** Short ack / error for chat-blob control responses. */
Object ChatBlobAckToJson(bool ok, const std::string& error = {});

} // namespace pbr
