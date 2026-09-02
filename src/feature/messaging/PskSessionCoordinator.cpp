#include "feature/messaging/PskSessionCoordinator.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/PskBundleCodec.h"
#include "base/messaging/E2eRelayPayloadCodec.h"
#include "common/thread/ThreadTypes.h"
#include "common/PbrCompat.h"

namespace pbr {

PskSessionCoordinator::PskSessionCoordinator(IThreadStore& store, IPskSessionStore& psk_store)
    : store_(store), psk_store_(psk_store) {}

Roe<ChatTargetKey> PskSessionCoordinator::TargetKeyForThread(const std::string& thread_id) const {
  auto thread = store_.GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  if ((*thread)->channel != ThreadChannel::E2e) {
    return Error("PSK setup applies to private E2E threads only");
  }
  return E2eRelayPayloadCodec::ChatTargetFromThread(**thread);
}

Roe<PskSessionStatus> PskSessionCoordinator::GetStatus(const std::string& thread_id) const {
  auto key = TargetKeyForThread(thread_id);
  if (!key) {
    return key.error();
  }
  PskSessionStatus status;
  if (auto epoch = store_.GetChatTargetSessionEpoch(thread_id)) {
    status.session_epoch = *epoch;
  }
  auto loaded = psk_store_.Load(*key);
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->has_value() || !loaded->value().master_psk_b64) {
    return status;
  }
  status.has_psk = true;
  status.verified = loaded->value().psk_verified_at.has_value();
  if (loaded->value().psk_fingerprint) {
    status.fingerprint = *loaded->value().psk_fingerprint;
  }
  status.session_epoch = loaded->value().session_epoch;
  return status;
}

Roe<PskExportView> PskSessionCoordinator::EnsureGenerated(const std::string& thread_id) {
  auto key = TargetKeyForThread(thread_id);
  if (!key) {
    return key.error();
  }
  if (auto loaded = psk_store_.Load(*key); loaded && loaded->has_value() && loaded->value().master_psk_b64) {
    return GetExportView(thread_id);
  }
  auto epoch = store_.GetChatTargetSessionEpoch(thread_id);
  if (!epoch) {
    return epoch.error();
  }

  auto generated = psk_store_.GenerateMasterPsk();
  if (!generated) {
    return generated.error();
  }

  PskSessionRecord record;
  record.key = *key;
  record.session_epoch = *epoch;
  record.master_psk_b64 = Base64Encode(*generated);
  record.psk_verified_at = std::nullopt;
  if (auto saved = psk_store_.Save(record); !saved) {
    return saved.error();
  }
  return GetExportView(thread_id);
}

Roe<PskExportView> PskSessionCoordinator::GetExportView(const std::string& thread_id) const {
  auto key = TargetKeyForThread(thread_id);
  if (!key) {
    return key.error();
  }
  auto loaded = psk_store_.Load(*key);
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->has_value() || !loaded->value().master_psk_b64) {
    return Error("PSK not configured");
  }
  PskExportView view;
  view.master_psk_b64 = *loaded->value().master_psk_b64;
  if (loaded->value().psk_fingerprint) {
    view.fingerprint = *loaded->value().psk_fingerprint;
  }
  return view;
}

Roe<std::string> PskSessionCoordinator::ExportBundleJson(const std::string& thread_id) const {
  auto key = TargetKeyForThread(thread_id);
  if (!key) {
    return key.error();
  }
  auto bundle = psk_store_.ExportPskBundle(*key);
  if (!bundle) {
    return bundle.error();
  }
  return PskBundleCodec::SerializeBundle(*bundle);
}

Roe<void> PskSessionCoordinator::ApplyBundleToThread(const std::string& thread_id, const ChatTargetKey& key,
                                                     const PskBundleV1& bundle) {
  auto current_epoch = store_.GetChatTargetSessionEpoch(thread_id);
  if (!current_epoch) {
    return current_epoch.error();
  }
  if (bundle.active_epoch > *current_epoch) {
    for (uint32_t epoch = *current_epoch; epoch < bundle.active_epoch; ++epoch) {
      if (auto cancelled = store_.CancelOldEpochPending(thread_id, epoch); !cancelled) {
        return cancelled.error();
      }
    }
  }

  if (auto imported = psk_store_.ImportPskBundle(key, bundle); !imported) {
    return imported.error();
  }
  if (bundle.active_epoch != *current_epoch) {
    if (auto adopted = store_.AdoptChatTargetEpoch(thread_id, bundle.active_epoch); !adopted) {
      return adopted.error();
    }
  }
  return {};
}

Roe<void> PskSessionCoordinator::ImportRawBase64(const std::string& thread_id, const std::string& raw_b64) {
  auto key = TargetKeyForThread(thread_id);
  if (!key) {
    return key.error();
  }
  auto decoded = PskBundleCodec::DecodeRawBase64(raw_b64);
  if (!decoded) {
    return decoded.error();
  }
  auto epoch = store_.GetChatTargetSessionEpoch(thread_id);
  if (!epoch) {
    return epoch.error();
  }

  PskBundleV1 bundle;
  bundle.channel = key->channel;
  bundle.active_epoch = *epoch;
  bundle.master_psk_b64 = decoded->master_psk_b64;
  return ApplyBundleToThread(thread_id, *key, bundle);
}

Roe<void> PskSessionCoordinator::ImportBundleJson(const std::string& thread_id, const std::string& bundle_json) {
  auto key = TargetKeyForThread(thread_id);
  if (!key) {
    return key.error();
  }
  auto bundle = PskBundleCodec::ParseBundleJson(bundle_json);
  if (!bundle) {
    return bundle.error();
  }
  if (bundle->channel != key->channel) {
    return Error("PSK bundle channel mismatch");
  }
  return ApplyBundleToThread(thread_id, *key, *bundle);
}

Roe<void> PskSessionCoordinator::MarkVerified(const std::string& thread_id, const int64_t verified_at_ms) {
  auto key = TargetKeyForThread(thread_id);
  if (!key) {
    return key.error();
  }
  return psk_store_.MarkPskVerified(*key, verified_at_ms);
}

Roe<std::string> PskSessionCoordinator::RotatePskAndExportBundle(const std::string& thread_id,
                                                                 const int64_t retired_at_ms) {
  auto key = TargetKeyForThread(thread_id);
  if (!key) {
    return key.error();
  }
  auto loaded = psk_store_.Load(*key);
  if (!loaded || !loaded->has_value() || !loaded->value().master_psk_b64) {
    return Error("No PSK to rotate");
  }

  PskSessionRecord record = loaded->value();
  const uint32_t old_epoch = record.session_epoch;
  if (auto cancelled = store_.CancelOldEpochPending(thread_id, old_epoch); !cancelled) {
    return cancelled.error();
  }

  RetiredPskEntry retired;
  retired.epoch = old_epoch;
  retired.master_psk_b64 = *record.master_psk_b64;
  retired.retired_at = retired_at_ms;
  record.retired_psks.push_back(std::move(retired));
  PskBundleCodec::CapRetiredTail(record.retired_psks, old_epoch + 1);

  auto new_psk = psk_store_.GenerateMasterPsk();
  if (!new_psk) {
    return new_psk.error();
  }
  record.master_psk_b64 = Base64Encode(*new_psk);
  record.session_epoch = old_epoch + 1;
  record.psk_verified_at = std::nullopt;
  if (auto saved = psk_store_.Save(record); !saved) {
    return saved.error();
  }
  if (auto adopted = store_.AdoptChatTargetEpoch(thread_id, record.session_epoch); !adopted) {
    return adopted.error();
  }

  auto bundle = psk_store_.ExportPskBundle(*key);
  if (!bundle) {
    return bundle.error();
  }
  return PskBundleCodec::SerializeBundle(*bundle);
}

} // namespace pbr
