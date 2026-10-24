#pragma once

#include "foundation/i18n/LocalizationService.h"

#include <string>

namespace pbr {

/** Map stable `payment_unavailable:` errors to localized user copy (P001). */
inline std::string PaymentErrorUserMessage(const std::string& technical) {
  if (technical.find("payment_unavailable:media_relay") != std::string::npos) {
    return Tr("call.error.payment_unavailable_media");
  }
  if (technical.find("payment_unavailable") != std::string::npos) {
    return Tr("call.error.payment_unavailable");
  }
  return technical;
}

} // namespace pbr
