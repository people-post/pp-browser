#include "lib/delivery/IPeerSessionStore.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/delivery/PskBundleCodec.h"

#include <sodium.h>

namespace pbr {

// What a peer session's rows mean (CR024): delivery's, over the store's Load / Save.

Roe<ByteVector> IPeerSessionStore::GenerateMasterPsk() const {
  EnsureSodiumInit();
  ByteVector psk(kMasterPskSize);
  randombytes_buf(psk.data(), psk.size());
  return psk;
}

Roe<std::optional<std::string>> IPeerSessionStore::ResolveMasterPskForEpoch(const PeerSessionKey& key,
                                                                              const uint32_t envelope_epoch) const {
  auto loaded = Load(key);
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->has_value()) {
    return std::optional<std::string>{};
  }
  const PeerSessionRecord& record = **loaded;
  if (envelope_epoch == record.session_epoch && record.master_psk_b64) {
    return std::optional<std::string>(*record.master_psk_b64);
  }
  for (const RetiredPskEntry& retired : record.retired_psks) {
    if (retired.epoch == envelope_epoch) {
      return std::optional<std::string>(retired.master_psk_b64);
    }
  }
  return std::optional<std::string>{};
}

Roe<void> IPeerSessionStore::MarkPskVerified(const PeerSessionKey& key, const int64_t verified_at_ms) {
  auto loaded = Load(key);
  if (!loaded || !loaded->has_value()) {
    return Error("PSK session not found");
  }
  PeerSessionRecord record = **loaded;
  record.psk_verified_at = verified_at_ms;
  return Save(record);
}

Roe<bool> IPeerSessionStore::IsPskVerified(const PeerSessionKey& key) const {
  auto loaded = Load(key);
  if (!loaded) {
    return loaded.error();
  }
  return loaded->has_value() && loaded->value().psk_verified_at.has_value();
}

Roe<PskBundleV1> IPeerSessionStore::ExportPskBundle(const PeerSessionKey& key) const {
  auto loaded = Load(key);
  if (!loaded || !loaded->has_value() || !loaded->value().master_psk_b64) {
    return Error("No PSK to export");
  }
  const PeerSessionRecord& record = loaded->value();
  PskBundleV1 bundle;
  bundle.channel = key.channel;
  bundle.active_epoch = record.session_epoch;
  bundle.master_psk_b64 = *record.master_psk_b64;
  bundle.retired_epochs = record.retired_psks;
  PskBundleCodec::CapRetiredTail(bundle.retired_epochs, bundle.active_epoch);
  if (auto valid = PskBundleCodec::ValidateBundle(bundle); !valid) {
    return valid.error();
  }
  return bundle;
}

Roe<void> IPeerSessionStore::ImportPskBundle(const PeerSessionKey& key, const PskBundleV1& bundle) {
  if (bundle.channel != key.channel) {
    return Error("PSK bundle channel mismatch");
  }
  if (auto valid = PskBundleCodec::ValidateBundle(bundle); !valid) {
    return valid.error();
  }

  PeerSessionRecord record;
  record.key = key;
  record.session_epoch = bundle.active_epoch;
  record.master_psk_b64 = bundle.master_psk_b64;
  record.psk_verified_at = std::nullopt;
  if (auto existing = Load(key); existing && existing->has_value()) {
    record.retired_psks = PskBundleCodec::MergeRetired(existing->value().retired_psks, bundle.retired_epochs);
    record.key_scope = existing->value().key_scope;
    record.thread_kem_pk_b64 = existing->value().thread_kem_pk_b64;
    record.thread_kem_sk_b64 = existing->value().thread_kem_sk_b64;
    record.peer_thread_kem_pk_b64 = existing->value().peer_thread_kem_pk_b64;
    record.last_psk_rotate_at = existing->value().last_psk_rotate_at;
    record.psk_rotate_msg_count = existing->value().psk_rotate_msg_count;
    record.last_rotation_id = existing->value().last_rotation_id;
  } else {
    record.retired_psks = bundle.retired_epochs;
  }
  PskBundleCodec::CapRetiredTail(record.retired_psks, record.session_epoch);
  return Save(record);
}

} // namespace pbr
