#pragma once

#include "foundation/data/PaymentPromiseTypes.h"
#include "common/thread/ThreadTypes.h"
#include "common/Error.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** System-message control types for exchanging signed payment-promise receipts (P002). */
enum class PaymentPromiseControlType {
  PromiseOffer,
  PromiseAccept,
  PromiseOutcome,
};

/**
 * Wire codec for payment-promise control messages.
 * `detail` carries the full PaymentPromise JSON from PaymentPromiseCodec::Encode.
 */
class PaymentPromiseWireCodec {
public:
  static std::string ControlTypeToWire(PaymentPromiseControlType type);
  static std::optional<PaymentPromiseControlType> ControlTypeFromWire(const std::string& value);
  static std::optional<PaymentPromiseControlType> ControlTypeFromMessage(const ThreadMessage& message);

  static Roe<std::string> EncodeDetail(const PaymentPromise& promise);
  static Roe<PaymentPromise> DecodeDetail(const std::string& detail_json);

  static Roe<ThreadMessage> BuildSystemMessage(const std::string& thread_id, PaymentPromiseControlType type,
                                               const PaymentPromise& promise, const std::string& body_text);
};

} // namespace pbr
