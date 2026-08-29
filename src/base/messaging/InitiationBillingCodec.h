#pragma once

#include "base/data/PricingTypes.h"
#include "base/messaging/ThreadTypes.h"
#include "common/Error.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

enum class InitiationBillingControlType {
  ChargeRequired,
  /** Initiator announces offer on first message path (optional companion to call invite fields). */
  InitiationOffer,
  /** Recipient decision: waive or take_all. */
  InitiationAccept,
};

struct ChargeRequiredDetail {
  std::string peer_identity;
  int64_t floor_minor = 0;
  std::string currency = kPricingCurrencyId;
  std::string message;
};

struct InitiationOfferDetail {
  std::string peer_identity;
  int64_t offer_minor = 0;
  int64_t floor_minor = 0;
  std::string currency = kPricingCurrencyId;
};

struct InitiationAcceptDetail {
  std::string peer_identity;
  int64_t offer_minor = 0;
  InitiationChargeDecision decision = InitiationChargeDecision::Waive;
  std::string currency = kPricingCurrencyId;
};

class InitiationBillingCodec {
public:
  static std::string ControlTypeToWire(InitiationBillingControlType type);
  static std::optional<InitiationBillingControlType> ControlTypeFromWire(const std::string& value);
  static std::optional<InitiationBillingControlType> ControlTypeFromMessage(const ThreadMessage& message);

  static Roe<std::string> EncodeChargeRequired(const ChargeRequiredDetail& detail);
  static Roe<ChargeRequiredDetail> DecodeChargeRequired(const std::string& detail_json);

  static Roe<std::string> EncodeInitiationOffer(const InitiationOfferDetail& detail);
  static Roe<InitiationOfferDetail> DecodeInitiationOffer(const std::string& detail_json);

  static Roe<std::string> EncodeInitiationAccept(const InitiationAcceptDetail& detail);
  static Roe<InitiationAcceptDetail> DecodeInitiationAccept(const std::string& detail_json);

  static Roe<ThreadMessage> BuildSystemMessage(const std::string& thread_id, InitiationBillingControlType type,
                                               const std::string& detail_json, const std::string& body_text);
};

} // namespace pbr
