#pragma once

#include "common/Error.h"
#include "common/thread/ChatBlobTypes.h"
#include "common/thread/ChatHistoryTypes.h"
#include "foundation/crypto/SignedReceive.h"
#include "lib/delivery/TypedSigning.h"
#include "lib/people/AccountCardRequest.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class IPeerSigningKeyResolver;

/**
 * Peer-direct (Amp) history / chat-blob / account-card requests name who asks. The Amp link authenticates a device
 * (its PeerId), not an account: the requester signs the request with its account key, over its fields, its own device
 * PeerId and the time. The responder serves the signer's thread only, and only on the link of the PeerId it signed (a
 * copied request does not work from another peer).
 *
 * v2 (release-compatibility SIGNED_FORMATS § 2.7), the request's `auth_version` outside and the byte after the domain
 * inside:
 *
 *   "pp-browser:peer-request" 00 ‖ u8 2 ‖ LenUtf8 kind ("history" | "blob" | "account-card")
 *   ‖ LenUtf8 requester_account_id ‖ LenUtf8 requester_peer_id ‖ i64 timestamp_ms ‖ (the kind's fields) ‖ ext area
 *
 * with a typed signature. JSON: `auth_version`, `ext_b64` (left out when empty) and `signature` beside the other auth
 * fields — at the top level of a history or blob request, in an account-card request's `auth` object.
 */
namespace PeerRequestAuth {

/** Release 1's floor and only version. */
inline constexpr int64_t kVersion = 2;
inline constexpr char kDomain[] = "pp-browser:peer-request";

/** How far a request's signed time may be from ours. */
inline constexpr int64_t kMaxSkewMs = 5 * 60 * 1000;

Roe<std::vector<uint8_t>> SignBytes(const ChatHistoryRequest& request);
Roe<std::vector<uint8_t>> SignBytes(const ChatBlobRequest& request);
Roe<std::vector<uint8_t>> SignBytes(const AccountCardRequest& request);

/** The account's raw signer (`IdentityStore::SignBytes`): Sign types the signature. */
using SignFn = RawSignFn;

/** Fill the auth fields (version 2, an empty extension area) and sign, as `account_id` on the link of `local_peer_id`. */
Roe<void> Sign(ChatHistoryRequest& request, const std::string& account_id, const std::string& local_peer_id,
               int64_t now_ms, const SignFn& sign);
Roe<void> Sign(ChatBlobRequest& request, const std::string& account_id, const std::string& local_peer_id,
               int64_t now_ms, const SignFn& sign);
Roe<void> Sign(AccountCardRequest& request, const std::string& account_id, const std::string& local_peer_id,
               int64_t now_ms, const SignFn& sign);

/** The requester's signing key as the people layer holds it (TypedSigning.h), looked up only when step 3 needs it. */
using SigningKeySource = std::function<Roe<std::string>()>;

/**
 * Steps 1–5 of the receiver's order (SIGNED_FORMATS § 1.1): `auth_version` (missing or below 2: Older; above: NeedsUpdate,
 * nothing verified), the auth fields and the extension area (Malformed), the signature's and the key's algorithm ids
 * (unknown: NeedsUpdate), the signature over the rebuilt bytes (Forged), the area's critical entries (none understood
 * at release 1: NeedsUpdate). The request's own checks (its link, its time) are Verify's.
 */
SignedReceipt Receive(const ChatHistoryRequest& request, const SigningKeySource& signing_key_b64);
SignedReceipt Receive(const ChatBlobRequest& request, const SigningKeySource& signing_key_b64);
SignedReceipt Receive(const AccountCardRequest& request, const SigningKeySource& signing_key_b64);

/**
 * The receiver's order (SIGNED_FORMATS § 1.1), then the request's own checks: verified, from `remote_peer_id`, recent —
 * the request's `requester_account_id` may be trusted. A newer `auth_version`, an unknown algorithm id or (once the
 * signature verified) an unknown critical extension is `AppError::NewerFormat`: the server answers `update_needed` (UpdateNeededRefusalToJson, common/chat/MessagingJson.h).
 * Every other refusal is one plain error ("Request not authorized"), its reason in our log only.
 */
Roe<void> Verify(const ChatHistoryRequest& request, const std::string& remote_peer_id, int64_t now_ms,
                 IPeerSigningKeyResolver& keys);
Roe<void> Verify(const ChatBlobRequest& request, const std::string& remote_peer_id, int64_t now_ms,
                 IPeerSigningKeyResolver& keys);
/** As above; the requester's own key card in the request, when it proves itself, stands in for a lookup. */
Roe<void> Verify(const AccountCardRequest& request, const std::string& remote_peer_id, int64_t now_ms,
                 IPeerSigningKeyResolver& keys);

} // namespace PeerRequestAuth

} // namespace pbr
