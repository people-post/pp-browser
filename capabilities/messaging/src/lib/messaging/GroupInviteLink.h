#pragma once

#include "lib/messaging/GroupRosterStatement.h"

#include "foundation/crypto/CryptoTypes.h"
#include "common/Error.h"

#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** G021: the text form of an invite link. */
inline constexpr const char kGroupJoinLinkPrefix[] = "pp://join/";

/**
 * An invite link (G021): a random id and secret, its expiry and the account of the member who made it — nothing
 * about the group. Text: `pp://join/` + base64url (unpadded) of link_id[16] ‖ secret[16] ‖ expires_at (u64 BE, ms) ‖
 * the maker's account id (UTF-8, the rest of the bytes).
 */
struct GroupJoinLink {
  /** The 16 id bytes as lowercase hex: the `link_id` the roster's `link_add` carries. */
  std::string link_id;
  /** 16 bytes; only its hash (SecretHash) enters the roster. */
  ByteVector secret;
  /** Unix ms. */
  int64_t expires_at = 0;
  /** The maker's account id (`account:…`): where a join request goes. */
  std::string maker;
};

namespace group_invite_link {

inline constexpr size_t kLinkIdBytes = 16;
inline constexpr size_t kSecretBytes = 16;
/** G021 2: a link lives 7 days unless its maker says otherwise. */
inline constexpr int64_t kDefaultLifetimeMs = 7LL * 24 * 60 * 60 * 1000;
/** A maker's account id in a link, at most (an account id is ~50 bytes). */
inline constexpr size_t kMaxMakerBytes = 128;

/** A new link of `maker`'s, expiring at `expires_at`: a random id and secret (libsodium). */
GroupJoinLink Mint(const std::string& maker, int64_t expires_at);

/** `pp://join/…`. */
std::string Encode(const GroupJoinLink& link);

/**
 * The first `pp://join/…` in `text` (pasted, scanned or opened), decoded: an error when there is none or it is
 * malformed (short, a maker that is not an account id, …).
 */
Roe<GroupJoinLink> Find(const std::string& text);

/**
 * What the roster keeps of a link's secret: lowercase hex of BLAKE2b-256 over the tag
 * `pp-browser:group-link-secret-v1`, the link id and the secret (each a u32 BE length then its bytes). A link's
 * answerer checks a join request's secret against it, so any admin can check one without the secret.
 */
std::string SecretHash(const std::string& link_id, const ByteVector& secret);

/** `secret` is the one `link`'s hash was made from (compared in constant time). */
bool SecretMatches(const GroupInviteLink& link, const ByteVector& secret);
/** Two secrets are the same (constant time); an empty one matches nothing. */
bool SecretsEqual(const ByteVector& a, const ByteVector& b);

} // namespace group_invite_link

} // namespace pbr
