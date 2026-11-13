#pragma once

#include "common/Error.h"
#include "common/chat/RelayGrant.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/** Signs bytes with this account's ML-DSA key (standard base64): IdentityStore::SignBytes. */
using AccountBytesSigner = std::function<Roe<std::string>(const std::vector<uint8_t>& bytes)>;

/** A grant this account issues is renewed once less than this is left of it (20 days into its 30). */
inline constexpr int64_t kRelayGrantRenewBeforeMs = 10LL * 24 * 60 * 60 * 1000;
/** A grant we hold is attached to a send only while at least this is left of it (the relay's clock may run ahead). */
inline constexpr int64_t kRelayGrantSendMarginMs = 10LL * 60 * 1000;

/**
 * What a grant's signature covers (multi-relay MR005; the relay's `buildRelayGrantSignBytes`):
 * `"pp-browser:relay-grant-v1" 0x00 | u8(1) | str recipient_account_id | str sender_account_id | i64be issued_at |
 * i64be expires_at`, `str` = u64be length + UTF-8, times Unix ms. The signature itself is not covered.
 */
std::vector<uint8_t> BuildRelayGrantSignBytes(const RelayGrant& grant);

/**
 * A grant from `recipient_account_id` (this account: `sign` signs with its key) to `sender_account_id`, issued at
 * `now_ms` and good for kRelayGrantMaxLifetimeMs.
 */
Roe<RelayGrant> IssueRelayGrant(const std::string& recipient_account_id, const std::string& sender_account_id,
                                int64_t now_ms, const AccountBytesSigner& sign);

/**
 * Whether `grant` is one `recipient_account_id` gave `sender_account_id` that a relay takes: the pair is the one
 * expected, its lifetime is at most kRelayGrantMaxLifetimeMs, it has not expired at `now_ms`, `recipient_public_key_b64`
 * is the recipient's account key (it hashes to its Account ID) and the signature is that key's.
 */
Roe<void> CheckRelayGrant(const RelayGrant& grant, const std::string& recipient_account_id,
                          const std::string& sender_account_id, const std::string& recipient_public_key_b64,
                          int64_t now_ms);

/** A grant we hold goes on a send while more than kRelayGrantSendMarginMs is left of it. */
bool RelayGrantUsable(const RelayGrant& grant, int64_t now_ms);

/** A grant we issued (expiring at `expires_at`) is due to be renewed: less than kRelayGrantRenewBeforeMs is left. */
bool RelayGrantDueForRenewal(int64_t expires_at, int64_t now_ms);

} // namespace pbr
