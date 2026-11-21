#include "lib/delivery/RelayGrantSigner.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/SignedReceive.h"
#include "foundation/crypto/TypedBlob.h"
#include "foundation/error/AppError.h"
#include "lib/people/CardKeys.h"

namespace pbr {

namespace {

/** Non-empty, no control characters (incl. NUL) and no DEL: account ids are stored and bound as text. */
bool IsPlainIdentifier(const std::string& value) {
  if (value.empty()) {
    return false;
  }
  for (const unsigned char c : value) {
    if (c < 0x20 || c == 0x7F) {
      return false;
    }
  }
  return true;
}

} // namespace

Object RelayGrantToJson(const RelayGrant& grant) {
  Object json;
  json.set(kRelayGrantVersionKey, int64_t{kRelayGrantVersion});
  json.set("recipient_account_id", grant.recipient_account_id);
  json.set("sender_account_id", grant.sender_account_id);
  json.set("issued_at", grant.issued_at);
  json.set("expires_at", grant.expires_at);
  WriteExtB64(json, grant.ext);
  json.set("signature", grant.signature);
  return json;
}

Roe<RelayGrant> RelayGrantFromJson(const Object& json) {
  // 1. The version, before anything else is read.
  const std::optional<int64_t> version = OutsideVersionOfJson(json, kRelayGrantVersionKey);
  if (!version || *version < kRelayGrantVersion) {
    return Error("Relay grant made by an older app (no grant_version 2)");
  }
  if (*version > kRelayGrantVersion) {
    return AppError::NewerFormat("Relay grant version " + std::to_string(*version) + ": made by a newer app");
  }
  // 2. The fields and the extension area.
  RelayGrant grant;
  grant.recipient_account_id = json.getString("recipient_account_id").value_or("");
  grant.sender_account_id = json.getString("sender_account_id").value_or("");
  grant.issued_at = json.getIf<int64_t>("issued_at").value_or(0);
  grant.expires_at = json.getIf<int64_t>("expires_at").value_or(0);
  grant.signature = json.getString("signature").value_or("");
  if (!IsPlainIdentifier(grant.recipient_account_id) || !IsPlainIdentifier(grant.sender_account_id) ||
      grant.issued_at <= 0 || grant.expires_at <= grant.issued_at || grant.signature.empty()) {
    return Error("Invalid relay grant");
  }
  auto ext = ReadExtB64(json);
  if (!ext) {
    return Error("Invalid relay grant: " + ext.error().message);
  }
  grant.ext = std::move(ext.value());
  // 3. The signature's algorithm id.
  if (const TypedRead signature = ReadTypedBlobB64(pp::BlobKind::kSignature, grant.signature, "relay grant signature");
      !signature.ok()) {
    return signature.ToError();
  }
  return grant;
}

namespace {

constexpr SignedVersions kGrantVersions{.floor = kRelayGrantVersion, .current = kRelayGrantVersion};

std::vector<uint8_t> SignBytesAt(const uint8_t version, const RelayGrant& grant) {
  return SignedBytesWriter(kRelayGrantDomain, version)
      .Utf8(grant.recipient_account_id)
      .Utf8(grant.sender_account_id)
      .I64(grant.issued_at)
      .I64(grant.expires_at)
      .Finish(grant.ext);
}

/** Step 6: the terms a relay also checks. */
Roe<void> CheckTerms(const RelayGrant& grant, const std::string& recipient_account_id,
                     const std::string& sender_account_id, const int64_t now_ms) {
  if (grant.recipient_account_id != recipient_account_id || grant.sender_account_id != sender_account_id) {
    return Error("Relay grant is for another pair");
  }
  if (grant.issued_at <= 0 || grant.expires_at <= grant.issued_at ||
      grant.expires_at - grant.issued_at > kRelayGrantMaxLifetimeMs) {
    return Error("Relay grant lifetime is invalid");
  }
  if (grant.expires_at <= now_ms) {
    return Error("Relay grant has expired");
  }
  return {};
}

} // namespace

std::vector<uint8_t> BuildRelayGrantSignBytes(const RelayGrant& grant) {
  return SignBytesAt(kRelayGrantVersion, grant);
}

Roe<RelayGrant> IssueRelayGrant(const std::string& recipient_account_id, const std::string& sender_account_id,
                                const int64_t now_ms, const AccountBytesSigner& sign) {
  if (recipient_account_id.empty() || sender_account_id.empty() || recipient_account_id == sender_account_id) {
    return Error("A relay grant is from one account to another");
  }
  if (now_ms <= 0 || !sign) {
    return Error("A relay grant needs a time and the account's key");
  }
  RelayGrant grant{.recipient_account_id = recipient_account_id,
                   .sender_account_id = sender_account_id,
                   .issued_at = now_ms,
                   .expires_at = now_ms + kRelayGrantMaxLifetimeMs};
  auto signature = sign(BuildRelayGrantSignBytes(grant));
  if (!signature) {
    return signature.error();
  }
  auto typed = TypedSignatureB64(*signature);
  if (!typed) {
    return typed.error();
  }
  grant.signature = std::move(typed.value());
  return grant;
}

SignedReceipt ReadSignedRelayGrant(const Object& json, const std::string& recipient_public_key_b64,
                                   RelayGrant* received) {
  const std::optional<int64_t> version = OutsideVersionOfJson(json, kRelayGrantVersionKey);
  RelayGrant grant;
  TypedRead key;
  TypedRead signature;
  const SignedReceipt receipt = ReceiveSigned({
      .what = "relay grant",
      .outside_version = version,
      .versions = kGrantVersions,
      .parse = [&]() -> Roe<void> {
        auto read = RelayGrantFromJson(json);
        if (!read) {
          return read.error();
        }
        grant = std::move(read.value());
        return {};
      },
      .algorithms = [&]() -> Roe<void> {
        signature = ReadTypedBlobB64(pp::BlobKind::kSignature, grant.signature, "relay grant signature");
        if (!signature.ok()) {
          return signature.ToError();
        }
        key =
            ReadTypedBlobB64(pp::BlobKind::kSignaturePublicKey, recipient_public_key_b64, "relay grant recipient key");
        if (!key.ok()) {
          return key.ToError();
        }
        return {};
      },
      .verify = [&]() -> Roe<void> {
        auto account = AccountIdFromMlDsaPublicKey(key.raw);
        if (!account || *account != grant.recipient_account_id) {
          return Error("the key is not the recipient's");
        }
        return VerifyTyped(key, SignBytesAt(static_cast<uint8_t>(*version), grant), signature);
      },
      .ext = &grant.ext,
  });
  if (receipt.accepted() && received != nullptr) {
    *received = std::move(grant);
  }
  return receipt;
}

Roe<RelayGrant> ReceiveRelayGrant(const Object& json, const std::string& recipient_account_id,
                                  const std::string& sender_account_id, const std::string& recipient_public_key_b64,
                                  const int64_t now_ms) {
  RelayGrant grant;
  if (const SignedReceipt receipt = ReadSignedRelayGrant(json, recipient_public_key_b64, &grant); !receipt.accepted()) {
    return receipt.ToError();
  }
  if (auto terms = CheckTerms(grant, recipient_account_id, sender_account_id, now_ms); !terms) {
    return terms.error();
  }
  return grant;
}

Roe<void> CheckRelayGrant(const RelayGrant& grant, const std::string& recipient_account_id,
                          const std::string& sender_account_id, const std::string& recipient_public_key_b64,
                          const int64_t now_ms) {
  auto received = ReceiveRelayGrant(RelayGrantToJson(grant), recipient_account_id, sender_account_id,
                                    recipient_public_key_b64, now_ms);
  if (!received) {
    return received.error();
  }
  return {};
}

bool RelayGrantUsable(const RelayGrant& grant, const int64_t now_ms) {
  return grant.expires_at - kRelayGrantSendMarginMs > now_ms;
}

bool RelayGrantDueForRenewal(const int64_t expires_at, const int64_t now_ms) {
  return expires_at - kRelayGrantRenewBeforeMs <= now_ms;
}

AccountStateGrant RelayGrantToLocker(const RelayGrant& grant) {
  Object json = RelayGrantToJson(grant);
  return AccountStateGrant{.grantor_account_id = grant.recipient_account_id,
                           .grantee_account_id = grant.sender_account_id,
                           .issued_at = grant.issued_at,
                           .expires_at = grant.expires_at,
                           .ext_b64 = json.getString(kExtB64Key).value_or(""),
                           .signature = grant.signature};
}

Roe<RelayGrant> RelayGrantFromLocker(const AccountStateGrant& grant) {
  return RelayGrantFromJson(AccountStateGrantToJson(grant));
}

} // namespace pbr
