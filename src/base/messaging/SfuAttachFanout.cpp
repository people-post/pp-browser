#include "base/messaging/SfuAttachFanout.h"

namespace pbr {

CallSfuAttachDetail BuildSfuAttachFanout(const CallSfuAttachDetail& after_local_attach) {
  CallSfuAttachDetail fanout = after_local_attach;
  fanout.quote_id.clear();
  return fanout;
}

uint32_t PublisherStreamIdForIdentity(const std::string& identity) {
  if (identity.empty()) {
    return 1u;
  }
  uint32_t h = 2166136261u;
  for (unsigned char c : identity) {
    h ^= c;
    h *= 16777619u;
  }
  return h == 0 ? 1u : h;
}

} // namespace pbr
