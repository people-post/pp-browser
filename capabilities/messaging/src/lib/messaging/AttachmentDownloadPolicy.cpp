#include "lib/messaging/AttachmentDownloadPolicy.h"

#include <algorithm>
#include <cctype>

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
                                 const uint64_t smart_max_bytes, const bool network_allows_all_auto) {
  switch (policy) {
  case AttachmentDownloadPolicy::AlwaysAuto:
    return network_allows_all_auto || byte_length <= smart_max_bytes;
  case AttachmentDownloadPolicy::OnDemand:
    return false;
  case AttachmentDownloadPolicy::Smart:
  default:
    return byte_length <= smart_max_bytes;
  }
}

AttachmentDownloadPolicy EffectiveAttachmentDownloadPolicy(const AttachmentDownloadPolicy policy,
                                                           const bool auto_on_wifi, const bool auto_on_cellular) {
  if (policy == AttachmentDownloadPolicy::AlwaysAuto && !auto_on_wifi && !auto_on_cellular) {
    return AttachmentDownloadPolicy::Smart;
  }
  return policy;
}

AutoDownloadNetwork AutoDownloadNetworkOf(const NetworkState& state, const bool mobile_host) {
  if (!mobile_host) {
    return AutoDownloadNetwork::Wifi;
  }
  if (!state.online || state.transport == NetworkTransport::Unknown) {
    return AutoDownloadNetwork::Unknown;
  }
  if (state.transport == NetworkTransport::Wifi && !state.expensive) {
    return AutoDownloadNetwork::Wifi;
  }
  return AutoDownloadNetwork::Mobile;
}

bool AutoDownloadAllowedOn(const AutoDownloadNetwork network, const bool auto_on_wifi, const bool auto_on_cellular) {
  switch (network) {
  case AutoDownloadNetwork::Wifi:
    return auto_on_wifi;
  case AutoDownloadNetwork::Mobile:
    return auto_on_cellular;
  case AutoDownloadNetwork::Unknown:
  default:
    return false;
  }
}

bool IsDeclaredVoiceMessage(const std::string& mime, const std::string& filename, const uint64_t byte_length) {
  constexpr uint64_t kVoiceMaxBytes = 1000ULL * 1000ULL; // lib/voice kVoiceBubbleMaxBytes
  std::string lowered = mime;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lowered != "audio/ogg" || byte_length == 0 || byte_length > kVoiceMaxBytes) {
    return false;
  }
  static const std::string kPrefix = "voice-";
  static const std::string kSuffix = ".ogg";
  if (filename.size() <= kPrefix.size() + kSuffix.size() || filename.compare(0, kPrefix.size(), kPrefix) != 0 ||
      filename.compare(filename.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
    return false;
  }
  const std::string digits = filename.substr(kPrefix.size(), filename.size() - kPrefix.size() - kSuffix.size());
  return digits.size() <= 9 &&
         std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

} // namespace pbr
