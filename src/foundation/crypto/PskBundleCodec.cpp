#include "foundation/crypto/PskBundleCodec.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <unordered_set>
#include "common/PbrCompat.h"

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
  auto parsed = TryParseObject(json);
  if (!parsed) {
    return Error("Invalid PSK bundle JSON");
  }
  if (parsed->getString("format").value_or(std::string{}) != "pp-browser-psk-bundle-v1") {
    return Error("Unsupported PSK bundle format");
  }

  PskBundleV1 bundle;
  bundle.channel = ChannelFromBundleString(parsed->getString("channel").value_or(std::string{"e2e"}));
  bundle.active_epoch = static_cast<uint32_t>(parsed->getNonNegInt("active_epoch").value_or(0));
  bundle.master_psk_b64 = parsed->getString("master_psk_b64").value_or(std::string{});
  if (bundle.active_epoch == 0) {
    return Error("PSK bundle missing active_epoch");
  }
  if (auto valid = ValidateMasterPskB64(bundle.master_psk_b64); !valid) {
    return valid.error();
  }

  if (const Array* retired_epochs = parsed->getArray("retired_epochs")) {
    uint32_t last_epoch = 0;
    std::unordered_set<uint32_t> seen;
    for (const Value& item_value : retired_epochs->elements) {
      const Object* item = asObject(item_value);
      if (!item) {
        continue;
      }
      RetiredPskEntry entry;
      entry.epoch = static_cast<uint32_t>(item->getNonNegInt("epoch").value_or(0));
      entry.master_psk_b64 = item->getString("master_psk_b64").value_or(std::string{});
      entry.retired_at = item->getIf<int64_t>("retired_at").value_or(0);
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
  Object json;
  json.set("format", "pp-browser-psk-bundle-v1");
  json.set("channel", CryptoChannelToString(bundle.channel));
  json.setJsonUInt("active_epoch", bundle.active_epoch);
  json.set("master_psk_b64", bundle.master_psk_b64);
  std::vector<Value> retired;
  retired.reserve(bundle.retired_epochs.size());
  for (const RetiredPskEntry& entry : bundle.retired_epochs) {
    Object item;
    item.setJsonUInt("epoch", entry.epoch);
    item.set("master_psk_b64", entry.master_psk_b64);
    item.set("retired_at", entry.retired_at);
    retired.push_back(ObjectValue(std::move(item)));
  }
  json.set("retired_epochs", ArrayValue(std::move(retired)));
  const std::string serialized = DumpJson(json);
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
