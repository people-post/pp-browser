#include "base/crypto/PskBundleCodec.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <unordered_set>

namespace pbr {

namespace {

std::string TrimAscii(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\n' || text.front() == '\r' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

CryptoChannel ChannelFromBundleString(const std::string& value) {
  return value == "e2e_public" ? CryptoChannel::E2ePublic : CryptoChannel::E2e;
}

Roe<void> ValidateMasterPskB64(const std::string& master_psk_b64) {
  auto bytes = Base64Decode(master_psk_b64);
  if (!bytes) {
    return bytes.error();
  }
  if (bytes->size() != kMasterPskSize) {
    return Error("PSK must decode to 32 bytes");
  }
  return {};
}

} // namespace

Roe<PskRawImport> PskBundleCodec::DecodeRawBase64(const std::string_view text) {
  const std::string trimmed = TrimAscii(text);
  if (trimmed.empty()) {
    return Error("Empty PSK paste");
  }
  auto bytes = Base64Decode(trimmed);
  if (!bytes) {
    return bytes.error();
  }
  if (bytes->size() != kMasterPskSize) {
    return Error("PSK must decode to 32 bytes");
  }
  PskRawImport result;
  result.master_psk = std::move(*bytes);
  result.master_psk_b64 = trimmed;
  return result;
}

Roe<PskBundleV1> PskBundleCodec::ParseBundleJson(const std::string& json) {
  if (json.size() > kMaxPskBundleBytes) {
    return Error("PSK bundle exceeds size limit");
  }
  const nlohmann::json parsed = nlohmann::json::parse(json, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return Error("Invalid PSK bundle JSON");
  }
  if (parsed.value("format", std::string{}) != "pp-browser-psk-bundle-v1") {
    return Error("Unsupported PSK bundle format");
  }

  PskBundleV1 bundle;
  bundle.channel = ChannelFromBundleString(parsed.value("channel", std::string{"e2e"}));
  bundle.active_epoch = parsed.value("active_epoch", 0u);
  bundle.master_psk_b64 = parsed.value("master_psk_b64", std::string{});
  if (bundle.active_epoch == 0) {
    return Error("PSK bundle missing active_epoch");
  }
  if (auto valid = ValidateMasterPskB64(bundle.master_psk_b64); !valid) {
    return valid.error();
  }

  if (parsed.contains("retired_epochs") && parsed["retired_epochs"].is_array()) {
    uint32_t last_epoch = 0;
    std::unordered_set<uint32_t> seen;
    for (const auto& item : parsed["retired_epochs"]) {
      RetiredPskEntry entry;
      entry.epoch = item.value("epoch", 0u);
      entry.master_psk_b64 = item.value("master_psk_b64", std::string{});
      entry.retired_at = item.value("retired_at", static_cast<int64_t>(0));
      if (entry.epoch == 0 || entry.epoch >= bundle.active_epoch) {
        return Error("Invalid retired epoch in bundle");
      }
      if (!seen.insert(entry.epoch).second) {
        return Error("Duplicate retired epoch in bundle");
      }
      if (entry.epoch <= last_epoch) {
        return Error("Retired epochs must be strictly increasing");
      }
      last_epoch = entry.epoch;
      if (auto valid = ValidateMasterPskB64(entry.master_psk_b64); !valid) {
        return valid.error();
      }
      bundle.retired_epochs.push_back(std::move(entry));
    }
    if (bundle.retired_epochs.size() > kMaxRetiredPskEpochs) {
      return Error("PSK bundle retired tail exceeds limit");
    }
  }

  if (auto valid = ValidateBundle(bundle); !valid) {
    return valid.error();
  }
  return bundle;
}

Roe<std::string> PskBundleCodec::SerializeBundle(const PskBundleV1& bundle) {
  if (auto valid = ValidateBundle(bundle); !valid) {
    return valid.error();
  }
  nlohmann::json json;
  json["format"] = "pp-browser-psk-bundle-v1";
  json["channel"] = CryptoChannelToString(bundle.channel);
  json["active_epoch"] = bundle.active_epoch;
  json["master_psk_b64"] = bundle.master_psk_b64;
  nlohmann::json retired = nlohmann::json::array();
  for (const RetiredPskEntry& entry : bundle.retired_epochs) {
    retired.push_back({{"epoch", entry.epoch},
                       {"master_psk_b64", entry.master_psk_b64},
                       {"retired_at", entry.retired_at}});
  }
  json["retired_epochs"] = std::move(retired);
  const std::string serialized = json.dump();
  if (serialized.size() > kMaxPskBundleBytes) {
    return Error("Serialized PSK bundle exceeds size limit");
  }
  return serialized;
}

Roe<void> PskBundleCodec::ValidateBundle(const PskBundleV1& bundle) {
  if (bundle.active_epoch == 0) {
    return Error("PSK bundle missing active_epoch");
  }
  if (auto valid = ValidateMasterPskB64(bundle.master_psk_b64); !valid) {
    return valid.error();
  }
  if (bundle.retired_epochs.size() > kMaxRetiredPskEpochs) {
    return Error("PSK bundle retired tail exceeds limit");
  }
  uint32_t last_epoch = 0;
  std::unordered_set<uint32_t> seen;
  for (const RetiredPskEntry& entry : bundle.retired_epochs) {
    if (entry.epoch == 0 || entry.epoch >= bundle.active_epoch) {
      return Error("Invalid retired epoch in bundle");
    }
    if (!seen.insert(entry.epoch).second) {
      return Error("Duplicate retired epoch in bundle");
    }
    if (entry.epoch <= last_epoch) {
      return Error("Retired epochs must be strictly increasing");
    }
    last_epoch = entry.epoch;
    if (auto valid = ValidateMasterPskB64(entry.master_psk_b64); !valid) {
      return valid.error();
    }
  }
  return {};
}

void PskBundleCodec::CapRetiredTail(std::vector<RetiredPskEntry>& retired, const uint32_t active_epoch) {
  retired.erase(std::remove_if(retired.begin(), retired.end(),
                               [active_epoch](const RetiredPskEntry& entry) { return entry.epoch >= active_epoch; }),
                retired.end());
  std::sort(retired.begin(), retired.end(),
            [](const RetiredPskEntry& a, const RetiredPskEntry& b) { return a.epoch < b.epoch; });
  if (retired.size() <= kMaxRetiredPskEpochs) {
    return;
  }
  retired.erase(retired.begin(), retired.end() - static_cast<ptrdiff_t>(kMaxRetiredPskEpochs));
}

std::vector<RetiredPskEntry> PskBundleCodec::MergeRetired(const std::vector<RetiredPskEntry>& existing,
                                                          const std::vector<RetiredPskEntry>& incoming) {
  std::vector<RetiredPskEntry> merged = existing;
  for (const RetiredPskEntry& entry : incoming) {
    auto it = std::find_if(merged.begin(), merged.end(),
                           [&](const RetiredPskEntry& candidate) { return candidate.epoch == entry.epoch; });
    if (it == merged.end()) {
      merged.push_back(entry);
    } else {
      *it = entry;
    }
  }
  std::sort(merged.begin(), merged.end(),
            [](const RetiredPskEntry& a, const RetiredPskEntry& b) { return a.epoch < b.epoch; });
  return merged;
}

} // namespace pbr
