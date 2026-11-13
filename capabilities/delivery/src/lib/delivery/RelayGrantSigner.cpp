#include "lib/delivery/RelayGrantSigner.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "lib/net/RelaySignBytes.h"

#include <sstream>

namespace pbr {

namespace {

constexpr const char kRelayGrantDomain[] = "pp-browser:relay-grant-v1";
constexpr uint8_t kRelayGrantSignVersion = 1;

} // namespace

std::vector<uint8_t> BuildRelayGrantSignBytes(const RelayGrant& grant) {
  std::ostringstream oss;
  RelaySignAppendDomain(oss, kRelayGrantDomain);
  RelaySignAppendU8(oss, kRelayGrantSignVersion);
  RelaySignAppendWireLenUtf8(oss, grant.recipient_account_id);
  RelaySignAppendWireLenUtf8(oss, grant.sender_account_id);
  RelaySignAppendI64(oss, grant.issued_at);
  RelaySignAppendI64(oss, grant.expires_at);
  return RelaySignOssToBytes(oss);
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
  grant.signature = *signature;
  return grant;
}

Roe<void> CheckRelayGrant(const RelayGrant& grant, const std::string& recipient_account_id,
                          const std::string& sender_account_id, const std::string& recipient_public_key_b64,
                          const int64_t now_ms) {
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
  auto public_key = Base64Decode(recipient_public_key_b64);
  if (!public_key || public_key->size() != kMlDsa65PublicKeyBytes) {
    return Error("Relay grant: no account key for the recipient");
  }
  auto account = AccountIdFromMlDsaPublicKey(*public_key);
  if (!account || *account != recipient_account_id) {
    return Error("Relay grant: the key is not the recipient's");
  }
  auto signature = Base64Decode(grant.signature);
  if (!signature) {
    return Error("Relay grant signature is not base64");
  }
  auto verified = MlDsa::Verify(*public_key, BuildRelayGrantSignBytes(grant), *signature);
  if (!verified || !*verified) {
    return Error("Relay grant signature does not verify");
  }
  return {};
}

bool RelayGrantUsable(const RelayGrant& grant, const int64_t now_ms) {
  return grant.expires_at - kRelayGrantSendMarginMs > now_ms;
}

bool RelayGrantDueForRenewal(const int64_t expires_at, const int64_t now_ms) {
  return expires_at - kRelayGrantRenewBeforeMs <= now_ms;
}

} // namespace pbr
