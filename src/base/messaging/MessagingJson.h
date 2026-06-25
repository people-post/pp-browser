#pragma once

#include "contacts/ContactTypes.h"
#include "messaging/ThreadTypes.h"

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace pbr {

std::string ThreadKindToString(ThreadKind kind);
ThreadKind ThreadKindFromString(const std::string& value);

std::string MessageDeliveryToString(MessageDelivery delivery);
MessageDelivery MessageDeliveryFromString(const std::string& value);

nlohmann::json ThreadToJson(const Thread& thread);
Thread ThreadFromJson(const nlohmann::json& json);

nlohmann::json ThreadMessageToJson(const ThreadMessage& message);
ThreadMessage ThreadMessageFromJson(const nlohmann::json& json);

nlohmann::json RelayEnvelopeToJson(const RelayEnvelope& envelope);
RelayEnvelope RelayEnvelopeFromJson(const nlohmann::json& json);

std::string ContactIdKindToString(ContactIdKind kind);
ContactIdKind ContactIdKindFromString(const std::string& value);

std::string TrustLevelToString(TrustLevel level);
TrustLevel TrustLevelFromString(const std::string& value);

nlohmann::json ContactToJson(const Contact& contact);
Contact ContactFromJson(const nlohmann::json& json);

nlohmann::json DirectoryHitToJson(const DirectoryHit& hit);
DirectoryHit DirectoryHitFromJson(const nlohmann::json& json);

} // namespace pbr
