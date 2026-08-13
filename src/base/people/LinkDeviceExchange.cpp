#include "base/people/LinkDeviceExchange.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"

#include <optional>

namespace pbr {

namespace {

Roe<void> DropPrivatePsks(std::vector<LinkDevicePublicPsk>& rows) {
  for (const LinkDevicePublicPsk& row : rows) {
    if (row.key.channel != CryptoChannel::E2ePublic) {
      return Error("Link export must not include private e2e PSKs");
    }
  }
  return {};
}

} // namespace

Roe<LinkDeviceBundleV1> LinkDeviceExchange::Capture(const LocalIdentity& identity, const ByteVector& dek,
                                                    std::vector<LinkDevicePublicPsk> public_psks, const int64_t now_ms,
                                                    const int64_t ttl_ms) {
  if (identity.account_id.empty() || identity.account_signing_private_key_b64.empty() ||
      identity.account_signing_public_key_b64.empty()) {
    return Error("Local account identity unavailable");
  }
  if (identity.relay_user_id.empty()) {
    return Error("Local relay identity unavailable");
  }
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  if (now_ms <= 0 || ttl_ms <= 0) {
    return Error("Invalid link-device lifetime");
  }
  if (auto filtered = DropPrivatePsks(public_psks); !filtered) {
    return filtered.error();
  }

  LinkDeviceBundleV1 bundle;
  bundle.account_id = identity.account_id;
  bundle.account_ml_dsa_pk_b64 = identity.account_signing_public_key_b64;
  bundle.account_ml_dsa_sk_b64 = identity.account_signing_private_key_b64;
  bundle.dek_b64 = Base64Encode(dek);
  bundle.relay_user_id = identity.relay_user_id;
  bundle.nickname = identity.nickname;
  bundle.created_at_ms = now_ms;
  bundle.expires_at_ms = now_ms + ttl_ms;
  bundle.public_psks = std::move(public_psks);
  if (auto valid = LinkDeviceCodec::Validate(bundle, now_ms); !valid) {
    return valid.error();
  }
  return bundle;
}

Roe<std::string> LinkDeviceExchange::ExportJson(const LocalIdentity& identity, const ByteVector& dek,
                                                std::vector<LinkDevicePublicPsk> public_psks, const int64_t now_ms,
                                                const int64_t ttl_ms) {
  auto bundle = Capture(identity, dek, std::move(public_psks), now_ms, ttl_ms);
  if (!bundle) {
    return bundle.error();
  }
  return LinkDeviceCodec::Serialize(*bundle);
}

Roe<LinkDeviceImportResult> LinkDeviceExchange::Import(IdentityStore& identity, DataKeyVault& vault,
                                                       const std::string& bundle_json, const std::string_view pin,
                                                       const int64_t now_ms) {
  auto parsed = LinkDeviceCodec::Parse(bundle_json);
  if (!parsed) {
    return parsed.error();
  }
  if (auto valid = LinkDeviceCodec::Validate(*parsed, now_ms); !valid) {
    return valid.error();
  }

  auto dek = Base64Decode(parsed->dek_b64);
  if (!dek) {
    return dek.error();
  }

  const bool had_vault = vault.Exists();
  std::optional<LocalIdentity> loaded;
  if (had_vault) {
    auto existing = identity.Get();
    if (!existing) {
      return existing.error();
    }
    loaded = *existing;
    if (auto wrapped = vault.ReplaceWithDek(pin, *dek); !wrapped) {
      return wrapped.error();
    }
  } else if (auto wrapped = vault.CreateWithDek(pin, *dek); !wrapped) {
    return wrapped.error();
  }
  if (had_vault) {
    if (auto keep = identity.ReplaceDekKeepLoaded(*dek); !keep) {
      return keep.error();
    }
  } else {
    if (auto set = identity.SetDek(*dek); !set) {
      return set.error();
    }
    auto created = identity.LoadOrCreate();
    if (!created) {
      return created.error();
    }
    loaded = *created;
  }

  LocalIdentity next = *loaded;
  const std::string device_peer = next.peer_id;
  const std::string device_pk = next.public_key_b64;
  const std::string device_sk = next.private_key_b64;
  const std::string kem_pk = next.kem_public_key_b64;
  const std::string kem_sk = next.kem_private_key_b64;

  next.account_id = parsed->account_id;
  next.account_signing_public_key_b64 = parsed->account_ml_dsa_pk_b64;
  next.account_signing_private_key_b64 = parsed->account_ml_dsa_sk_b64;
  next.relay_user_id = parsed->relay_user_id;
  if (!parsed->nickname.empty()) {
    next.nickname = parsed->nickname;
  }
  next.registered = true;
  next.peer_id = device_peer;
  next.public_key_b64 = device_pk;
  next.private_key_b64 = device_sk;
  next.kem_public_key_b64 = kem_pk;
  next.kem_private_key_b64 = kem_sk;

  auto saved = identity.Update(next);
  if (!saved) {
    return saved.error();
  }

  LinkDeviceImportResult result;
  result.identity = *saved;
  result.public_psks = std::move(parsed->public_psks);
  return result;
}

} // namespace pbr
