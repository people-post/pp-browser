#include "domain/pricing/PricingControlFamilies.h"

#include "domain/pricing/InitiationBillingCodec.h"
#include "domain/pricing/InitiationBillingStore.h"
#include "domain/pricing/InitiationPricing.h"
#include "domain/pricing/PaymentPromiseStore.h"
#include "domain/pricing/PaymentPromiseWireCodec.h"

#include <optional>
#include <string>
#include <utility>
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::optional<std::string> DetailText(const ThreadMessage& message) {
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return std::nullopt;
  }
  auto detail = payload->getString("detail");
  if (!detail || detail->empty()) {
    return std::nullopt;
  }
  return detail;
}

/** The peer a billing message is about: its authenticated sender, which the payload may only echo. */
Roe<std::string> BillingPeer(const std::string& named, const SystemControlContext& context) {
  if (context.sender_identity.empty()) {
    return Error("Initiation billing control without a sender");
  }
  if (!named.empty() && named != context.sender_identity) {
    return Error("Initiation billing control names another peer");
  }
  return context.sender_identity;
}

Roe<void> ApplyBilling(InitiationBillingStore& store, const std::function<int64_t()>& local_floor,
                       const ThreadMessage& message, const SystemControlContext& context) {
  const auto type = InitiationBillingCodec::ControlTypeFromMessage(message);
  if (!type) {
    return {};
  }
  const auto detail = DetailText(message);
  if (!detail) {
    return Error("Initiation billing control missing detail");
  }
  switch (*type) {
  case InitiationBillingControlType::ChargeRequired: {
    auto charge = InitiationBillingCodec::DecodeChargeRequired(*detail);
    if (!charge) {
      return charge.error();
    }
    auto peer = BillingPeer(charge->peer_identity, context);
    if (!peer) {
      return peer.error();
    }
    (void)store.SetFloor(*peer, charge->floor_minor);
    (void)store.MarkClosed(*peer);
    return {};
  }
  case InitiationBillingControlType::InitiationOffer: {
    auto offer = InitiationBillingCodec::DecodeInitiationOffer(*detail);
    if (!offer) {
      return offer.error();
    }
    auto peer = BillingPeer(offer->peer_identity, context);
    if (!peer) {
      return peer.error();
    }
    const int64_t floor = local_floor ? local_floor() : 0;
    if (floor > 0 && !InitiationPricing::CheckOfferAgainstFloor(offer->offer_minor, floor)) {
      return {}; // Too low: dropped quietly, the sender sees no accept.
    }
    (void)store.MarkOffered(*peer, offer->offer_minor, floor > 0 ? floor : offer->floor_minor);
    return {};
  }
  case InitiationBillingControlType::InitiationAccept: {
    auto accept = InitiationBillingCodec::DecodeInitiationAccept(*detail);
    if (!accept) {
      return accept.error();
    }
    auto peer = BillingPeer(accept->peer_identity, context);
    if (!peer) {
      return peer.error();
    }
    // Waive or take-all both open the relationship; settlement comes later.
    (void)store.MarkOpen(*peer);
    return {};
  }
  }
  return {};
}

} // namespace

SystemControlFamily MakeInitiationBillingFamily(InitiationBillingStore& store, std::function<int64_t()> local_floor) {
  SystemControlFamily family;
  family.name = kInitiationBillingFamily;
  family.owns = [](const ThreadMessage& message) {
    return InitiationBillingCodec::ControlTypeFromMessage(message).has_value();
  };
  family.apply = [&store, local_floor = std::move(local_floor)](ThreadMessage& message,
                                                                const SystemControlContext& context) {
    return ApplyBilling(store, local_floor, message, context);
  };
  return family;
}

SystemControlFamily MakePaymentPromiseFamily(PaymentPromiseStore& store) {
  SystemControlFamily family;
  family.name = kPaymentPromiseFamily;
  family.owns = [](const ThreadMessage& message) {
    return PaymentPromiseWireCodec::ControlTypeFromMessage(message).has_value();
  };
  family.apply = [&store](ThreadMessage& message, const SystemControlContext& context) -> Roe<void> {
    const auto detail = DetailText(message);
    if (!detail) {
      return Error("payment promise control missing detail");
    }
    auto decoded = PaymentPromiseWireCodec::DecodeDetail(*detail);
    if (!decoded) {
      return decoded.error();
    }
    if (context.sender_identity.empty() || (decoded->payer_account_id != context.sender_identity &&
                                            decoded->payee_account_id != context.sender_identity)) {
      return Error("payment promise from someone not a party to it");
    }
    auto staged = store.StageInbound(*decoded);
    if (!staged) {
      return staged.error();
    }
    return {};
  };
  return family;
}

} // namespace pbr
