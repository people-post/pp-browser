#include "lib/messaging/GroupInviteLink.h"

#include "foundation/crypto/CryptoUtil.h"

#include <cctype>
#include <cstdint>
#include <sodium.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace group_invite_link {
namespace {

constexpr const char kSecretHashTag[] = "pp-browser:group-link-secret-v1";
constexpr size_t kFixedBytes = kLinkIdBytes + kSecretBytes + 8;

bool IsBase64UrlChar(const char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
}

void Put(ByteVector& out, const ByteVector& field) {
  const auto n = static_cast<uint32_t>(field.size());
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(n >> shift));
  }
  out.insert(out.end(), field.begin(), field.end());
}

void Put(ByteVector& out, const std::string& field) {
  Put(out, ByteVector(field.begin(), field.end()));
}

ByteVector RandomBytes(const size_t n) {
  EnsureSodiumInit();
  ByteVector bytes(n);
  randombytes_buf(bytes.data(), bytes.size());
  return bytes;
}

} // namespace

GroupJoinLink Mint(const std::string& maker, const int64_t expires_at) {
  return {.link_id = BytesToHex(RandomBytes(kLinkIdBytes)),
          .secret = RandomBytes(kSecretBytes),
          .expires_at = expires_at,
          .maker = maker};
}

std::string Encode(const GroupJoinLink& link) {
  ByteVector bytes;
  if (auto id = HexToBytes(link.link_id); id && id->size() == kLinkIdBytes) {
    bytes = std::move(*id);
  } else {
    bytes.assign(kLinkIdBytes, 0);
  }
  bytes.insert(bytes.end(), link.secret.begin(), link.secret.end());
  const auto expires = static_cast<uint64_t>(link.expires_at < 0 ? 0 : link.expires_at);
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<uint8_t>(expires >> shift));
  }
  bytes.insert(bytes.end(), link.maker.begin(), link.maker.end());
  return std::string(kGroupJoinLinkPrefix) + Base64UrlEncode(bytes);
}

Roe<GroupJoinLink> Find(const std::string& text) {
  const size_t at = text.find(kGroupJoinLinkPrefix);
  if (at == std::string::npos) {
    return Error("No invite link");
  }
  const size_t start = at + std::char_traits<char>::length(kGroupJoinLinkPrefix);
  size_t end = start;
  while (end < text.size() && IsBase64UrlChar(text[end])) {
    ++end;
  }
  auto bytes = Base64UrlDecode(text.substr(start, end - start));
  if (!bytes) {
    return Error("Invite link does not decode");
  }
  const ByteVector& b = *bytes;
  if (b.size() <= kFixedBytes || b.size() > kFixedBytes + kMaxMakerBytes) {
    return Error("Invite link has the wrong length");
  }
  GroupJoinLink link;
  link.link_id = BytesToHex(ByteVector(b.begin(), b.begin() + kLinkIdBytes));
  link.secret.assign(b.begin() + kLinkIdBytes, b.begin() + kLinkIdBytes + kSecretBytes);
  uint64_t expires = 0;
  for (size_t i = kLinkIdBytes + kSecretBytes; i < kFixedBytes; ++i) {
    expires = (expires << 8) | b[i];
  }
  if (expires == 0 || expires > static_cast<uint64_t>(INT64_MAX)) {
    return Error("Invite link has no expiry");
  }
  link.expires_at = static_cast<int64_t>(expires);
  link.maker.assign(b.begin() + kFixedBytes, b.end());
  if (link.maker.rfind("account:", 0) != 0 || link.maker.size() == std::char_traits<char>::length("account:")) {
    return Error("Invite link names no account");
  }
  for (const char c : link.maker) {
    if (!std::isgraph(static_cast<unsigned char>(c))) {
      return Error("Invite link names no account");
    }
  }
  return link;
}

std::string SecretHash(const std::string& link_id, const ByteVector& secret) {
  ByteVector bytes;
  Put(bytes, std::string(kSecretHashTag));
  Put(bytes, link_id);
  Put(bytes, secret);
  return BytesToHex(Blake2b256(bytes));
}

bool SecretMatches(const GroupInviteLink& link, const ByteVector& secret) {
  const std::string hash = SecretHash(link.link_id, secret);
  return hash.size() == link.secret_hash.size() &&
         sodium_memcmp(hash.data(), link.secret_hash.data(), hash.size()) == 0;
}

} // namespace group_invite_link
} // namespace pbr
