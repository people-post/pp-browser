#pragma once

#include "base/people/ContactTypes.h"
#include "base/messaging/ThreadTypes.h"

#include <nlohmann/json_fwd.hpp>
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

nlohmann::json ThreadToJson(const Thread& thread);
Thread ThreadFromJson(const nlohmann::json& json);

nlohmann::json ThreadMessageToJson(const ThreadMessage& message);
ThreadMessage ThreadMessageFromJson(const nlohmann::json& json);

nlohmann::json RelayEnvelopeToJson(const RelayEnvelope& envelope);
/** Application envelope JSON without relay routing fields (for blob encoding). */
nlohmann::json RelayEnvelopeToApplicationJson(const RelayEnvelope& envelope);
/** Strict ingest — rejects legacy thread_id / flat body.text (D063). */
Roe<RelayEnvelope> ParseRelayEnvelope(const nlohmann::json& json);

nlohmann::json RelayWireSendRecordToJson(const RelayWireSendRecord& record);
Roe<RelayWireSendRecord> RelayWireSendRecordFromEnvelope(const RelayEnvelope& envelope);
Roe<RelayEnvelope> RelayEnvelopeFromInboundRecord(const RelayInboundRecord& record);
Roe<RelayInboundRecord> ParseRelayInboundRecord(const nlohmann::json& json);

nlohmann::json ChatHistoryRequestToJson(const ChatHistoryRequest& request);
Roe<ChatHistoryRequest> ChatHistoryRequestFromJson(const nlohmann::json& json);
/** HTTP query string for GET /v1/streams/messages (generic relay). */
std::string StreamHistoryRequestToQueryString(const ChatHistoryRequest& request);
/** @deprecated Use StreamHistoryRequestToQueryString */
std::string ChatHistoryRequestToQueryString(const ChatHistoryRequest& request);
nlohmann::json ChatHistoryRequestToStreamHistoryJson(const ChatHistoryRequest& request);
nlohmann::json ChatHistoryResponseToJson(const ChatHistoryResponse& response);
Roe<ChatHistoryResponse> ChatHistoryResponseFromJson(const nlohmann::json& json);

std::string ContactIdKindToString(ContactIdKind kind);
ContactIdKind ContactIdKindFromString(const std::string& value);

std::string TrustLevelToString(TrustLevel level);
TrustLevel TrustLevelFromString(const std::string& value);

nlohmann::json ContactToJson(const Contact& contact);
Contact ContactFromJson(const nlohmann::json& json);

nlohmann::json DirectoryHitToJson(const DirectoryHit& hit);
DirectoryHit DirectoryHitFromJson(const nlohmann::json& json);

} // namespace pbr
