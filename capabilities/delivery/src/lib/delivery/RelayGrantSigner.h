#pragma once

#include "common/Error.h"
#include "common/Value.h"
#include "common/chat/RelayGrant.h"
#include "foundation/crypto/SignedReceive.h"
#include "lib/people/AccountStateCodec.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * Signs bytes with this account's ML-DSA key: IdentityStore::SignBytes (a bare ML-DSA-65 signature in standard
 * base64; a typed one is taken as it is).
 */
using AccountBytesSigner = std::function<Roe<std::string>(const std::vector<uint8_t>& bytes)>;

/**
 * `{grant_version: 2, recipient_account_id, sender_account_id, issued_at, expires_at, ext_b64 (when not empty),
 * signature}`.
 */
Object RelayGrantToJson(const RelayGrant& grant);

/**
 * Steps 1–3 of the receiver's order (docs/contracts/SIGNED_EXTENSIONS.md): `grant_version` first (missing or older:
 * refused; newer: `AppError::NewerFormat`, nothing else read), then the fields and the extension area (malformed:
 * refused), then the signature's algorithm id (unknown: NewerFormat). The signature itself is not checked here:
 * `ReceiveRelayGrant` runs the whole order with the recipient's key.
 */
Roe<RelayGrant> RelayGrantFromJson(const Object& json);

/** A grant this account issues is renewed once less than this is left of it (20 days into its 30). */
inline constexpr int64_t kRelayGrantRenewBeforeMs = 10LL * 24 * 60 * 60 * 1000;
/** A grant we hold is attached to a send only while at least this is left of it (the relay's clock may run ahead). */
inline constexpr int64_t kRelayGrantSendMarginMs = 10LL * 60 * 1000;

/** The grant's signing domain (no `-vN`: the version is the byte after it). */
inline constexpr char kRelayGrantDomain[] = "pp-browser:relay-grant";

/**
 * What a grant's signature covers (SIGNED_FORMATS § 2.4; the relay's grant check reads the same bytes):
 * `"pp-browser:relay-grant" 00 ‖ u8 2 ‖ LenUtf8 recipient_account_id ‖ LenUtf8 sender_account_id ‖ i64 issued_at ‖
 * i64 expires_at ‖ ext area` (u64 BE lengths, big-endian integers, times in Unix ms; the area is the u64 BE length of
 * the entries, then the entries). The signature itself is not covered.
 */
std::vector<uint8_t> BuildRelayGrantSignBytes(const RelayGrant& grant);

/**
 * A grant from `recipient_account_id` (this account: `sign` signs with its key) to `sender_account_id`, issued at
 * `now_ms` and good for kRelayGrantMaxLifetimeMs, with an empty extension area.
 */
Roe<RelayGrant> IssueRelayGrant(const std::string& recipient_account_id, const std::string& sender_account_id,
                                int64_t now_ms, const AccountBytesSigner& sign);

/**
 * Steps 1–5 of the receiver's order for a grant's JSON (docs/contracts/SIGNED_EXTENSIONS.md § The receiver's order):
 * `grant_version`, the fields and area, the algorithm ids of the signature and of `recipient_public_key_b64` (the key
 * as held: IdentityStore, a key card), the signature (by that key, which must hash to the recipient's Account ID), an
 * unknown critical entry. What the relay runs on a send's grant before its terms. `received` is filled when accepted.
 */
SignedReceipt ReadSignedRelayGrant(const Object& json, const std::string& recipient_public_key_b64,
                                   RelayGrant* received);

/**
 * Receive a grant's JSON in the receiver's order (docs/contracts/SIGNED_EXTENSIONS.md § The receiver's order):
 * `grant_version`, the fields and area, the signature's algorithm id, the signature (by `recipient_public_key_b64`,
 * which must be the recipient's account key: it hashes to its Account ID), an unknown critical entry; then the terms:
 * the pair is the one expected, its lifetime is at most kRelayGrantMaxLifetimeMs and it has not expired at `now_ms`.
 *
 * "Update needed" (a newer version, an unknown algorithm, an unknown critical entry) is `AppError::NewerFormat`: the
 * grant is treated as none and its grantor as one that needs a newer app. Any other error is a refusal.
 * `recipient_public_key_b64` is the key as held (IdentityStore, a key card).
 */
Roe<RelayGrant> ReceiveRelayGrant(const Object& json, const std::string& recipient_account_id,
                                  const std::string& sender_account_id, const std::string& recipient_public_key_b64,
                                  int64_t now_ms);

/** `ReceiveRelayGrant` of a grant already read (`RelayGrantFromJson`): the same checks, on its JSON. */
Roe<void> CheckRelayGrant(const RelayGrant& grant, const std::string& recipient_account_id,
                          const std::string& sender_account_id, const std::string& recipient_public_key_b64,
                          int64_t now_ms);

/** A grant we hold goes on a send while more than kRelayGrantSendMarginMs is left of it. */
bool RelayGrantUsable(const RelayGrant& grant, int64_t now_ms);

/** A grant we issued (expiring at `expires_at`) is due to be renewed: less than kRelayGrantRenewBeforeMs is left. */
bool RelayGrantDueForRenewal(int64_t expires_at, int64_t now_ms);

/** A grant given to us, as the account locker's `grants` keep it (its version, area and signature as they are). */
AccountStateGrant RelayGrantToLocker(const RelayGrant& grant);
/** A locker `grants` row read back as a grant (`RelayGrantFromJson` of its JSON: a newer one is NewerFormat). */
Roe<RelayGrant> RelayGrantFromLocker(const AccountStateGrant& grant);

} // namespace pbr
