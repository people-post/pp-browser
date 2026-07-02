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
/** Strict ingest — rejects legacy thread_id / flat body.text (D063). */
Roe<RelayEnvelope> ParseRelayEnvelope(const nlohmann::json& json);

nlohmann::json ChatHistoryRequestToJson(const ChatHistoryRequest& request);
Roe<ChatHistoryRequest> ChatHistoryRequestFromJson(const nlohmann::json& json);
/** HTTP query string for GET /v1/chat-targets/messages (D027). */
std::string ChatHistoryRequestToQueryString(const ChatHistoryRequest& request);
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
