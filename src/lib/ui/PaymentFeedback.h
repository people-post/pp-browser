#pragma once

#include "foundation/i18n/LocalizationService.h"

#include <string>

namespace pbr {

/**
 * Map stable send errors to localized user copy: `payment_unavailable:` (P001), and a group send refused
 * while the group checks its members, after we were removed or once the owner dissolved it (`chat.group.*` keys,
 * group-chat G013 / G016).
 */
inline std::string PaymentErrorUserMessage(const std::string& technical) {
  if (technical == "chat.group.checking_members" || technical == "chat.group.removed" ||
      technical == "chat.group.dissolved") {
    return Tr(technical);
  }
  if (technical.find("payment_unavailable:media_relay") != std::string::npos) {
    return Tr("call.error.payment_unavailable_media");
  }
  if (technical.find("payment_unavailable") != std::string::npos) {
    return Tr("call.error.payment_unavailable");
  }
  return technical;
}

} // namespace pbr
