#include "lib/people/LinkOffer.h"

#include "common/PbrCompat.h"

namespace pbr {

Roe<LinkOffer::Shown> LinkOffer::Begin(const int64_t now_ms) {
  auto keys = LinkOfferKeys::Generate();
  if (!keys) {
    return keys.error();
  }
  auto offer = LinkOfferBytes(keys->public_key());
  if (!offer) {
    return offer.error();
  }
  std::lock_guard lock(mutex_);
  ForgetLocked();
  keys_ = std::move(*keys);
  offer_ = std::move(*offer);
  expires_at_ms_ = now_ms + kLinkWindowMs;
  return Shown{.offer = offer_,
               .text = LinkPayloadText(kLinkOfferTextPrefix, offer_),
               .code = LinkConfirmationCode(offer_),
               .expires_at_ms = expires_at_ms_};
}

std::optional<LinkOffer::Shown> LinkOffer::Current(const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  if (!LiveLocked(now_ms)) {
    return std::nullopt;
  }
  return Shown{.offer = offer_,
               .text = LinkPayloadText(kLinkOfferTextPrefix, offer_),
               .code = LinkConfirmationCode(offer_),
               .expires_at_ms = expires_at_ms_};
}

Roe<LinkBundle> LinkOffer::Open(const std::span<const uint8_t> reply, const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  if (!LiveLocked(now_ms)) {
    return MakeLinkError(LinkError::Expired, "link: no one-time key (gone, or its 15 minutes passed)");
  }
  return OpenLinkReply(offer_, keys_, reply, now_ms);
}

void LinkOffer::Forget() {
  std::lock_guard lock(mutex_);
  ForgetLocked();
}

void LinkOffer::ForgetLocked() {
  keys_.Wipe();
  offer_.clear();
  expires_at_ms_ = 0;
}

bool LinkOffer::LiveLocked(const int64_t now_ms) {
  if (keys_.empty()) {
    return false;
  }
  if (now_ms >= expires_at_ms_) {
    ForgetLocked();
    return false;
  }
  return true;
}

} // namespace pbr
