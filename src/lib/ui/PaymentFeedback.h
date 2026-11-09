#pragma once

#include "common/Error.h"
#include "foundation/error/AppError.h"
#include "foundation/i18n/LocalizationService.h"

#include <string>

namespace pbr {

/**
 * Map stable send errors to localized user copy: `payment_unavailable:` (P001), and a group send refused
 * while the group checks its members, after we were removed or once the owner dissolved it (`chat.group.*` keys,
 * group-chat G013 / G016).
 */
inline std::string PaymentErrorUserMessage(const std::string& technical) {
  // Stable codes only. Any other text is a developer's English, never shown: a generic line in the user's language.
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
  return Tr("error.generic");
}

/** The same for an error: the stable codes in its message, else the words it carries (AppError::Display). */
inline std::string PaymentErrorUserMessage(const Error& error) {
  const std::string mapped = PaymentErrorUserMessage(error.message);
  return mapped == Tr("error.generic") ? AppError::Display(error) : mapped;
}

} // namespace pbr
