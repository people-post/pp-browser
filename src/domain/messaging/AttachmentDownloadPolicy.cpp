#include "domain/messaging/AttachmentDownloadPolicy.h"

#include "common/chat/MessagingLimits.h"

namespace pbr {

std::string AttachmentDownloadPolicyToString(const AttachmentDownloadPolicy policy) {
  switch (policy) {
  case AttachmentDownloadPolicy::AlwaysAuto:
    return "always_auto";
  case AttachmentDownloadPolicy::OnDemand:
    return "on_demand";
  case AttachmentDownloadPolicy::Smart:
  default:
    return "smart";
  }
}

AttachmentDownloadPolicy AttachmentDownloadPolicyFromString(const std::string& value) {
  if (value == "always_auto") {
    return AttachmentDownloadPolicy::AlwaysAuto;
  }
  if (value == "on_demand") {
    return AttachmentDownloadPolicy::OnDemand;
  }
  return AttachmentDownloadPolicy::Smart;
}

bool ShouldAutoEnqueueAttachment(const AttachmentDownloadPolicy policy, const uint64_t byte_length,
                                 const bool backlog_drain) {
  if (backlog_drain) {
    return true;
  }
  switch (policy) {
  case AttachmentDownloadPolicy::AlwaysAuto:
    return true;
  case AttachmentDownloadPolicy::OnDemand:
    return false;
  case AttachmentDownloadPolicy::Smart:
  default:
    return byte_length <= kMaxChatAttachmentPlaintextBytes;
  }
}

} // namespace pbr
