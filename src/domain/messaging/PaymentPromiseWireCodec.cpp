#include "domain/messaging/PaymentPromiseWireCodec.h"

#include "domain/messaging/PaymentPromiseCodec.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

std::string PaymentPromiseWireCodec::ControlTypeToWire(const PaymentPromiseControlType type) {
  switch (type) {
  case PaymentPromiseControlType::PromiseOffer:
    return "promise_offer";
  case PaymentPromiseControlType::PromiseAccept:
    return "promise_accept";
  case PaymentPromiseControlType::PromiseOutcome:
    return "promise_outcome";
  }
  return "promise_offer";
}

std::optional<PaymentPromiseControlType> PaymentPromiseWireCodec::ControlTypeFromWire(const std::string& value) {
  if (value == "promise_offer") {
    return PaymentPromiseControlType::PromiseOffer;
  }
  if (value == "promise_accept") {
    return PaymentPromiseControlType::PromiseAccept;
  }
  if (value == "promise_outcome") {
    return PaymentPromiseControlType::PromiseOutcome;
  }
  return std::nullopt;
}

std::optional<PaymentPromiseControlType> PaymentPromiseWireCodec::ControlTypeFromMessage(
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

Roe<std::string> PaymentPromiseWireCodec::EncodeDetail(const PaymentPromise& promise) {
  return PaymentPromiseCodec::Encode(promise);
}

Roe<PaymentPromise> PaymentPromiseWireCodec::DecodeDetail(const std::string& detail_json) {
  return PaymentPromiseCodec::Decode(detail_json);
}

Roe<ThreadMessage> PaymentPromiseWireCodec::BuildSystemMessage(const std::string& thread_id,
                                                               const PaymentPromiseControlType type,
                                                               const PaymentPromise& promise,
                                                               const std::string& body_text) {
  auto detail = EncodeDetail(promise);
  if (!detail) {
    return detail.error();
  }
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
  payload.set("detail", *detail);
  message.payload_json = DumpJson(payload);
  return message;
}

} // namespace pbr
