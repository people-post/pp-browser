#include "lib/pricing/PricingControlFamilies.h"

#include "lib/pricing/InitiationBillingCodec.h"
#include "lib/pricing/InitiationBillingStore.h"
#include "lib/pricing/InitiationPricing.h"
#include "lib/pricing/PaymentPromiseStore.h"
#include "lib/pricing/PaymentPromiseWireCodec.h"

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

SystemControlFamily MakePaymentPromiseFamily(PaymentPromiseStore& store,
                                             std::function<std::optional<std::string>()> local_account,
                                             AccountSigningKeyLookup account_key) {
  SystemControlFamily family;
  family.name = kPaymentPromiseFamily;
  family.owns = [](const ThreadMessage& message) {
    return PaymentPromiseWireCodec::ControlTypeFromMessage(message).has_value();
  };
  family.apply = [&store, local_account = std::move(local_account), account_key = std::move(account_key)](
                     ThreadMessage& message, const SystemControlContext& context) -> Roe<void> {
    const auto detail = DetailText(message);
    if (!detail) {
      return Error("payment promise control missing detail");
    }
    auto incoming = PaymentPromiseWireCodec::DecodeDetail(*detail);
    if (!incoming) {
      return incoming.error();
    }
    const auto local = local_account ? local_account() : std::nullopt;
    const auto counterparty = local ? PaymentPromiseCounterparty(*incoming, *local) : std::nullopt;
    if (!counterparty || context.sender_identity != *counterparty) {
      return Error("payment promise not from our counterparty on it");
    }
    if (!account_key) {
      return Error("payment promise signer key unavailable");
    }
    auto key = account_key(*counterparty);
    if (!key) {
      return key.error();
    }
    auto committed = store.Get(incoming->promise_id);
    if (!committed) {
      return committed.error();
    }
    if (auto merged = MergeInboundPaymentPromise(*committed, *incoming, *local, *key); !merged) {
      return merged.error();
    }
    return store.StageInbound(*incoming);
  };
  return family;
}

} // namespace pbr
