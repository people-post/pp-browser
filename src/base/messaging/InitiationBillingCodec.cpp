#include "base/messaging/InitiationBillingCodec.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

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
  auto json = TryParseObject(message.payload_json);
  if (!json) {
    return std::nullopt;
  }
  auto control_type = json->getString("control_type");
  if (!control_type) {
    return std::nullopt;
  }
  return ControlTypeFromWire(*control_type);
}

Roe<std::string> InitiationBillingCodec::EncodeChargeRequired(const ChargeRequiredDetail& detail) {
  Object json;
  json.set("peer_identity", detail.peer_identity);
  json.set("floor_minor", detail.floor_minor);
  json.set("currency", detail.currency.empty() ? kPricingCurrencyId : detail.currency);
  if (!detail.message.empty()) {
    json.set("message", detail.message);
  }
  return DumpJson(json);
}

Roe<ChargeRequiredDetail> InitiationBillingCodec::DecodeChargeRequired(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  if (!json) {
    return Error("Invalid charge_required detail");
  }
  ChargeRequiredDetail detail;
  detail.peer_identity = json->getString("peer_identity").value_or("");
  detail.floor_minor = json->getIf<int64_t>("floor_minor").value_or(0);
  detail.currency = json->getString("currency").value_or(std::string(kPricingCurrencyId));
  detail.message = json->getString("message").value_or("");
  return detail;
}

Roe<std::string> InitiationBillingCodec::EncodeInitiationOffer(const InitiationOfferDetail& detail) {
  Object json;
  json.set("peer_identity", detail.peer_identity);
  json.set("offer_minor", detail.offer_minor);
  json.set("floor_minor", detail.floor_minor);
  json.set("currency", detail.currency.empty() ? kPricingCurrencyId : detail.currency);
  return DumpJson(json);
}

Roe<InitiationOfferDetail> InitiationBillingCodec::DecodeInitiationOffer(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  if (!json) {
    return Error("Invalid initiation_offer detail");
  }
  InitiationOfferDetail detail;
  detail.peer_identity = json->getString("peer_identity").value_or("");
  detail.offer_minor = json->getIf<int64_t>("offer_minor").value_or(0);
  detail.floor_minor = json->getIf<int64_t>("floor_minor").value_or(0);
  detail.currency = json->getString("currency").value_or(std::string(kPricingCurrencyId));
  return detail;
}

Roe<std::string> InitiationBillingCodec::EncodeInitiationAccept(const InitiationAcceptDetail& detail) {
  Object json;
  json.set("peer_identity", detail.peer_identity);
  json.set("offer_minor", detail.offer_minor);
  json.set("decision", InitiationChargeDecisionToWire(detail.decision));
  json.set("currency", detail.currency.empty() ? kPricingCurrencyId : detail.currency);
  return DumpJson(json);
}

Roe<InitiationAcceptDetail> InitiationBillingCodec::DecodeInitiationAccept(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  if (!json) {
    return Error("Invalid initiation_accept detail");
  }
  InitiationAcceptDetail detail;
  detail.peer_identity = json->getString("peer_identity").value_or("");
  detail.offer_minor = json->getIf<int64_t>("offer_minor").value_or(0);
  detail.decision = InitiationChargeDecisionFromWire(json->getString("decision").value_or("waive"));
  detail.currency = json->getString("currency").value_or(std::string(kPricingCurrencyId));
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
  Object payload;
  payload.set("control_type", ControlTypeToWire(type));
  payload.set("detail", detail_json);
  message.payload_json = DumpJson(payload);
  return message;
}

} // namespace pbr
