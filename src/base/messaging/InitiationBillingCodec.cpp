#include "base/messaging/InitiationBillingCodec.h"

#include "common/Utilities.h"

#include <nlohmann/json.hpp>

namespace pbr {

namespace {

nlohmann::json ParseObject(const std::string& detail_json) {
  return nlohmann::json::parse(detail_json, nullptr, false);
}

} // namespace

std::string InitiationBillingCodec::ControlTypeToWire(const InitiationBillingControlType type) {
  switch (type) {
  case InitiationBillingControlType::ChargeRequired:
    return "charge_required";
  case InitiationBillingControlType::InitiationOffer:
    return "initiation_offer";
  case InitiationBillingControlType::InitiationAccept:
    return "initiation_accept";
  }
  return "charge_required";
}

std::optional<InitiationBillingControlType> InitiationBillingCodec::ControlTypeFromWire(const std::string& value) {
  if (value == "charge_required") {
    return InitiationBillingControlType::ChargeRequired;
  }
  if (value == "initiation_offer") {
    return InitiationBillingControlType::InitiationOffer;
  }
  if (value == "initiation_accept") {
    return InitiationBillingControlType::InitiationAccept;
  }
  return std::nullopt;
}

std::optional<InitiationBillingControlType> InitiationBillingCodec::ControlTypeFromMessage(
    const ThreadMessage& message) {
  const nlohmann::json json = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!json.is_object() || !json.contains("control_type") || !json["control_type"].is_string()) {
    return std::nullopt;
  }
  return ControlTypeFromWire(json["control_type"].get<std::string>());
}

Roe<std::string> InitiationBillingCodec::EncodeChargeRequired(const ChargeRequiredDetail& detail) {
  nlohmann::json json{{"peer_identity", detail.peer_identity},
                      {"floor_minor", detail.floor_minor},
                      {"currency", detail.currency.empty() ? kPricingCurrencyId : detail.currency}};
  if (!detail.message.empty()) {
    json["message"] = detail.message;
  }
  return json.dump();
}

Roe<ChargeRequiredDetail> InitiationBillingCodec::DecodeChargeRequired(const std::string& detail_json) {
  const nlohmann::json json = ParseObject(detail_json);
  if (!json.is_object()) {
    return Error("Invalid charge_required detail");
  }
  ChargeRequiredDetail detail;
  detail.peer_identity = json.value("peer_identity", "");
  detail.floor_minor = json.value("floor_minor", static_cast<int64_t>(0));
  detail.currency = json.value("currency", std::string(kPricingCurrencyId));
  detail.message = json.value("message", "");
  return detail;
}

Roe<std::string> InitiationBillingCodec::EncodeInitiationOffer(const InitiationOfferDetail& detail) {
  return nlohmann::json{{"peer_identity", detail.peer_identity},
                        {"offer_minor", detail.offer_minor},
                        {"floor_minor", detail.floor_minor},
                        {"currency", detail.currency.empty() ? kPricingCurrencyId : detail.currency}}
      .dump();
}

Roe<InitiationOfferDetail> InitiationBillingCodec::DecodeInitiationOffer(const std::string& detail_json) {
  const nlohmann::json json = ParseObject(detail_json);
  if (!json.is_object()) {
    return Error("Invalid initiation_offer detail");
  }
  InitiationOfferDetail detail;
  detail.peer_identity = json.value("peer_identity", "");
  detail.offer_minor = json.value("offer_minor", static_cast<int64_t>(0));
  detail.floor_minor = json.value("floor_minor", static_cast<int64_t>(0));
  detail.currency = json.value("currency", std::string(kPricingCurrencyId));
  return detail;
}

Roe<std::string> InitiationBillingCodec::EncodeInitiationAccept(const InitiationAcceptDetail& detail) {
  return nlohmann::json{{"peer_identity", detail.peer_identity},
                        {"offer_minor", detail.offer_minor},
                        {"decision", InitiationChargeDecisionToWire(detail.decision)},
                        {"currency", detail.currency.empty() ? kPricingCurrencyId : detail.currency}}
      .dump();
}

Roe<InitiationAcceptDetail> InitiationBillingCodec::DecodeInitiationAccept(const std::string& detail_json) {
  const nlohmann::json json = ParseObject(detail_json);
  if (!json.is_object()) {
    return Error("Invalid initiation_accept detail");
  }
  InitiationAcceptDetail detail;
  detail.peer_identity = json.value("peer_identity", "");
  detail.offer_minor = json.value("offer_minor", static_cast<int64_t>(0));
  detail.decision = InitiationChargeDecisionFromWire(json.value("decision", "waive"));
  detail.currency = json.value("currency", std::string(kPricingCurrencyId));
  return detail;
}

Roe<ThreadMessage> InitiationBillingCodec::BuildSystemMessage(const std::string& thread_id,
                                                              const InitiationBillingControlType type,
                                                              const std::string& detail_json,
                                                              const std::string& body_text) {
  ThreadMessage message;
  message.id = util::GenerateUuid();
  message.thread_id = thread_id;
  message.content_type = ChatContentType::System;
  message.text = body_text;
  message.timestamp = util::NowUnixMs();
  message.delivery = MessageDelivery::Local;
  message.relay_visible = true;
  message.payload_json =
      nlohmann::json({{"control_type", ControlTypeToWire(type)}, {"detail", detail_json}}).dump();
  return message;
}

} // namespace pbr
