#include "lib/calls/CallMediaErrorText.h"

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

} // namespace

const char* CallMediaErrorUserKey(const std::string& technical) {
  // A sentence with non-ASCII characters is already user copy (a feature's Tr() line).
  if (technical.empty() || !IsAscii(technical)) {
    return nullptr;
  }
  if (Has(technical, "payment_unavailable")) {
    return nullptr;  // PaymentErrorUserMessage owns these
  }
  if (Has(technical, "no path within the reconnect window")) {
    return "call.error.reconnect_failed";
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
