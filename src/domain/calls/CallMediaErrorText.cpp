#include "domain/calls/CallMediaErrorText.h"

namespace pbr {

namespace {

bool Has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

/** Transport text: a layer prefix or a bracketed cause chain, never a sentence a feature wrote. */
bool LooksTechnical(const std::string& text) {
  return text.rfind("amp ", 0) == 0 || text.rfind("call-media", 0) == 0 || text.rfind("reach:", 0) == 0 ||
         Has(text, "[") || Has(text, "amp link") || Has(text, "circuit-relay");
}

} // namespace

const char* CallMediaErrorUserKey(const std::string& technical) {
  if (technical.empty()) {
    return nullptr;
  }
  if (Has(technical, "payment_unavailable")) {
    return nullptr;  // PaymentErrorUserMessage owns these
  }
  if (Has(technical, "no path within the reconnect window")) {
    return "call.error.reconnect_failed";
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
  if (LooksTechnical(technical)) {
    return "call.error.media_failed";
  }
  return nullptr;
}

} // namespace pbr
