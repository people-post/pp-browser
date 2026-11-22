#pragma once

#include "lib/calls/MediaKeyWrap.h"

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/SignedReceive.h"

#include "common/Error.h"
#include "common/ExtArea.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "common/PbrCompat.h"

namespace pbr {

/** The label broadcast show keys are wrapped under for a viewer (MediaKeyWrap; calls use their own). */
inline constexpr const char* kBroadcastMediaKeyWrapLabel = "broadcast_media_key";

/**
 * The ticket's version, outside (`schema_version`, compared before verifying) and inside (the byte after the domain):
 * 2 since the signed-format cutover (release-compatibility SIGNED_FORMATS § 2.9), the floor and the only version read.
 */
inline constexpr int kBroadcastJoinTicketSchemaVersion = 2;
inline constexpr char kBroadcastJoinTicketDomain[] = "pp-browser:broadcast-join-ticket";

/**
 * B0 / Spine F — publisher-signed grant that delivers the stable broadcast media key.
 * Frame AEAD stays encrypt-once (B003); every hop must forward opaque ciphertext.
 * Wire: docs/contracts/WIRE_SCHEMAS.md § BroadcastJoinTicket.
 */
struct BroadcastJoinTicket {
  int schema_version = kBroadcastJoinTicketSchemaVersion;
  std::string publisher_peer_id;
  std::string program_id;
  /** Opaque live session / call id (== tip.join_handle). */
  std::string join_handle;
  /** Bound viewer PeerId; reject if accept path uses a different identity. */
  std::string viewer_peer_id;
  uint32_t media_epoch = 1;
  std::string media_key_id;
  /**
   * Preferred: pairwise wrap under viewer↔publisher session key. A typed AEAD blob (MediaKeyWrap's: id 1,
   * xchacha20-poly1305, then nonce ‖ ciphertext ‖ tag), base64.
   */
  std::string wrapped_key_b64;
  /**
   * Lab / open path when no pairwise session yet: raw key bytes (base64), still
   * publisher-signed and viewer-bound. Prefer wrapped_key_b64 in production.
   */
  std::string key_material_b64;
  /** Optional leaf / SFU hop PeerId hint. */
  std::string hop_peer_id;
  int64_t expires_at_ms = 0;
  /** The signed extension area (`ext_b64`): empty at release 1; unknown non-critical entries are kept. */
  pp::ExtArea ext;
  /** The publisher device's typed signature (base64): its algorithm id, then the signature. */
  std::string signature_b64;
};

struct BroadcastMediaKey {
  std::string call_id;
  uint32_t media_epoch = 1;
  std::string media_key_id;
  ByteVector key_bytes;
};

struct BroadcastJoinTicketDraft {
  std::string publisher_peer_id;
  std::string program_id;
  std::string join_handle;
  std::string viewer_peer_id;
  uint32_t media_epoch = 1;
  /** Empty → mint assigns `mk:<hex>`. */
  std::string media_key_id;
  std::string hop_peer_id;
  int64_t expires_at_ms = 0;
};

/**
 * The signed bytes, rebuilt with `ticket.schema_version` as the inside version:
 *
 *   "pp-browser:broadcast-join-ticket" 00 ‖ u8 version ‖ LenUtf8 publisher_peer_id ‖ LenUtf8 program_id
 *   ‖ LenUtf8 join_handle ‖ LenUtf8 viewer_peer_id ‖ u64 media_epoch ‖ LenUtf8 media_key_id
 *   ‖ LenBytes wrapped_key (empty if none) ‖ LenBytes key_material (empty if none) ‖ LenUtf8 hop_peer_id ("" if none)
 *   ‖ i64 expires_at_ms ‖ ext area
 *
 * An error when the version is not a byte or a key field is not base64.
 */
Roe<ByteVector> BroadcastJoinTicketSignBytes(const BroadcastJoinTicket& ticket);

Roe<std::string> EncodeBroadcastJoinTicketJson(const BroadcastJoinTicket& ticket);
/**
 * Steps 1–2 of the receiver's order: `schema_version` first (newer: `AppError::NewerFormat`, update needed; older or
 * none: refused), then the fields and `ext_b64` (malformed: refused). Nothing is verified here.
 */
Roe<BroadcastJoinTicket> DecodeBroadcastJoinTicketJson(std::string_view json);

/**
 * Mint a signed ticket. If `viewer_pairwise_session_key` is non-null and non-empty,
 * wraps the media key (MediaKeyWrap, label `broadcast_media_key`). Otherwise fills `key_material_b64`.
 */
Roe<BroadcastJoinTicket> MintBroadcastJoinTicket(BroadcastJoinTicketDraft draft, const ByteVector& media_key_bytes,
                                                 const ByteVector& publisher_mldsa_secret_key,
                                                 const ByteVector* viewer_pairwise_session_key = nullptr);

/**
 * Steps 1–5 of the receiver's order (`ReceiveSigned`): version, fields, algorithm ids (the signature's, the wrapped
 * key's), the publisher's signature, the extension area. Update needed (a newer version, an unknown algorithm id, an
 * unknown critical entry once the signature holds) is never a forgery.
 */
SignedReceipt ReceiveBroadcastJoinTicket(const BroadcastJoinTicket& ticket,
                                         const ByteVector& publisher_mldsa_public_key);

/**
 * The whole order from the wire JSON (what the frozen vectors check): `schema_version` before any field, then as
 * `ReceiveBroadcastJoinTicket`. On Accepted, `*out` (when given) is the ticket read.
 */
SignedReceipt ReceiveBroadcastJoinTicketJson(std::string_view json, const ByteVector& publisher_mldsa_public_key,
                                             BroadcastJoinTicket* out = nullptr);

/**
 * `ReceiveBroadcastJoinTicket`, then step 6: expiry and the viewer. Update needed is `AppError::NewerFormat`, every
 * other refusal an Error.
 */
Roe<void> VerifyBroadcastJoinTicket(const BroadcastJoinTicket& ticket, const ByteVector& publisher_mldsa_public_key,
                                    int64_t now_ms, std::string_view expected_viewer_peer_id = {});

/**
 * Verify + extract plaintext media key bytes (unwrap or decode key_material).
 * Does not touch any key store.
 */
Roe<BroadcastMediaKey> ExtractBroadcastMediaKey(const BroadcastJoinTicket& ticket,
                                                const ByteVector& publisher_mldsa_public_key, int64_t now_ms,
                                                std::string_view expected_viewer_peer_id,
                                                const ByteVector* viewer_pairwise_session_key = nullptr);

} // namespace pbr
