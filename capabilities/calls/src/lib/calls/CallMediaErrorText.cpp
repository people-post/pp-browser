#include "lib/calls/CallMediaErrorText.h"

#include "common/Localized.h"

namespace pbr {

namespace {

bool Has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

bool IsAscii(const std::string& text) {
  for (const unsigned char c : text) {
    if (c >= 0x80) {
      return false;
    }
  }
  return true;
}

/** Transport text: a layer prefix, or a bracketed cause chain naming a layer; never a sentence a feature wrote. */
bool LooksTechnical(const std::string& text) {
  if (text.rfind("amp ", 0) == 0 || text.rfind("call-media", 0) == 0 || text.rfind("reach:", 0) == 0 ||
      text.rfind("link:", 0) == 0) {
    return true;
  }
  return Has(text, "[") && (Has(text, "amp") || Has(text, "reach") || Has(text, "link") || Has(text, "tunnel") ||
                            Has(text, "signaling") || Has(text, "relay") || Has(text, "circuit"));
}

/** The locale keys of the notices the calls feature words itself (the one place to extend). */
constexpr const char* kLocalizedNoticeKeys[] = {
    "call.notice.removed",
    "call.notice.left_group",
    "call.notice.call_ended",
    "call.error.media_unavailable",
};

} // namespace

bool CallNoticeIsLocalizedCopy(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  for (const char* key : kLocalizedNoticeKeys) {
    if (text == Localized(key)) {
      return true;
    }
  }
  return false;
}

bool CallMediaErrorShownOnCallScreen(const CallMediaError& error) {
  // K014: a call that was connected and lost its media for good shows "Call dropped" on the failed page; a 1:1
  // call that never connected within its budget shows "Couldn't reach them" there. Saying either again as a
  // toast or banner is a duplicate (#181); the raw reason stays in the log.
  return error.kind == CallMediaErrorKind::Dropped || error.kind == CallMediaErrorKind::ConnectTimeout;
}

const char* CallMediaErrorUserKey(const std::string& technical) {
  // A sentence with non-ASCII characters is already user copy (a feature's Localized() line).
  if (technical.empty() || !IsAscii(technical)) {
    return nullptr;
  }
  if (Has(technical, "payment_unavailable")) {
    return nullptr; // PaymentErrorUserMessage owns these
  }
  if (!LooksTechnical(technical)) {
    return nullptr;
  }
  if (Has(technical, "not reached") || Has(technical, "unreachable")) {
    return "call.error.peer_unreachable";
  }
  if (Has(technical, "migrate") && Has(technical, "timed out")) {
    return "call.error.migrate_timeout";
  }
  if (Has(technical, "too many concurrent dials") || Has(technical, "dial aborted") || Has(technical, "dial failed")) {
    return "call.error.dial_failed";
  }
  return "call.error.media_failed";
}

} // namespace pbr
