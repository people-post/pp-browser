#pragma once

#include "domain/messaging/CallMediaKeyStore.h"

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "common/PbrCompat.h"

namespace pbr {

inline constexpr int kBroadcastJoinTicketSchemaVersion = 1;

/**
 * B0 / Spine F — publisher-signed grant that delivers the stable broadcast media key.
 * Frame AEAD stays encrypt-once (B003); every hop must forward opaque ciphertext.
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
  /** Preferred: pairwise wrap under viewer↔publisher session key. */
  std::string wrapped_key_b64;
  /**
   * Lab / open path when no pairwise session yet: raw key bytes (base64), still
   * publisher-signed and viewer-bound. Prefer wrapped_key_b64 in production.
   */
  std::string key_material_b64;
  /** Optional leaf / SFU hop PeerId hint. */
  std::string hop_peer_id;
  int64_t expires_at_ms = 0;
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

std::string BroadcastJoinTicketCanonicalSignBytes(const BroadcastJoinTicket& ticket);

Roe<std::string> EncodeBroadcastJoinTicketJson(const BroadcastJoinTicket& ticket);
Roe<BroadcastJoinTicket> DecodeBroadcastJoinTicketJson(std::string_view json);

/**
 * Mint a signed ticket. If `viewer_pairwise_session_key` is non-null and non-empty,
 * wraps the media key (CallMediaKeyStore AAD). Otherwise fills `key_material_b64`.
 */
Roe<BroadcastJoinTicket> MintBroadcastJoinTicket(BroadcastJoinTicketDraft draft,
                                                 const ByteVector& media_key_bytes,
                                                 const ByteVector& publisher_mldsa_secret_key,
                                                 const ByteVector* viewer_pairwise_session_key = nullptr);

Roe<void> VerifyBroadcastJoinTicket(const BroadcastJoinTicket& ticket,
                                    const ByteVector& publisher_mldsa_public_key, int64_t now_ms,
                                    std::string_view expected_viewer_peer_id = {});

/**
 * Verify + extract plaintext media key bytes (unwrap or decode key_material).
 * Does not touch CallMediaKeyStore.
 */
Roe<BroadcastMediaKey> ExtractBroadcastMediaKey(const BroadcastJoinTicket& ticket,
                                                const ByteVector& publisher_mldsa_public_key,
                                                int64_t now_ms, std::string_view expected_viewer_peer_id,
                                                const ByteVector* viewer_pairwise_session_key = nullptr);

/** Verify, extract, and PutEpochKey into the local vault-backed store. */
Roe<BroadcastMediaKey> ApplyBroadcastJoinTicket(CallMediaKeyStore& store, const BroadcastJoinTicket& ticket,
                                                const ByteVector& publisher_mldsa_public_key, int64_t now_ms,
                                                std::string_view expected_viewer_peer_id,
                                                const ByteVector* viewer_pairwise_session_key = nullptr);

} // namespace pbr
