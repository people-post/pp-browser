#pragma once

#include "lib/broadcast/PeerAnnounceTypes.h"

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/SignedReceive.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

Roe<std::string> MakePeerAnnounceTopicId(std::string_view peer_id, std::string_view local_name,
                                         std::string_view app_ns = kPeerAnnounceAppNs);

/**
 * The signed bytes, rebuilt with `tip.schema_version` as the inside version; every field always (empty when absent):
 *
 *   "pp-browser:peer-announce-tip" 00 ‖ u8 version ‖ LenUtf8 peer_id ‖ LenUtf8 topic_id ‖ LenUtf8 program_id
 *   ‖ LenUtf8 state ‖ u64 seq ‖ u64 epoch ‖ i64 created_at_ms ‖ LenUtf8 join_handle ‖ LenUtf8 hop_peer_id
 *   ‖ u64 n ‖ n × LenUtf8 l1_hop_peer_id ‖ u64 n ‖ n × u8 video_level ‖ LenUtf8 kind ‖ LenUtf8 viewer_peer_id
 *   ‖ LenUtf8 viewer_msg_id ‖ LenUtf8 body ‖ LenUtf8 content_id_hex ‖ ext area
 *
 * An error when the version or a video level is not a byte.
 */
Roe<ByteVector> PeerAnnounceTipSignBytes(const PeerAnnounceTip& tip);

Roe<std::string> EncodePeerAnnounceTipJson(const PeerAnnounceTip& tip);
/**
 * Steps 1–2 of the receiver's order: `schema_version` first (newer: `AppError::NewerFormat`, update needed; older or
 * none: refused), then the fields and `ext_b64` (malformed: refused, a video level outside 1–15 too). Nothing is
 * verified here.
 */
Roe<PeerAnnounceTip> DecodePeerAnnounceTipJson(std::string_view json);

/** Sign with device ML-DSA-65 secret (PeerId-bound; matches IdentityStore device key): a typed signature. */
Roe<PeerAnnounceTip> SignPeerAnnounceTip(PeerAnnounceTip tip, const std::vector<uint8_t>& mldsa_secret_key);
/**
 * The receiver's order (`ReceiveSigned`): version, fields, the signature's algorithm id, the publisher's signature,
 * the extension area. Update needed (a newer version, an unknown algorithm id, an unknown critical entry once the
 * signature holds) is never a forgery.
 */
SignedReceipt ReceivePeerAnnounceTip(const PeerAnnounceTip& tip, const std::vector<uint8_t>& mldsa_public_key);
/**
 * The whole order from the wire JSON (what the frozen vectors check): `schema_version` before any field, then as
 * `ReceivePeerAnnounceTip`. On Accepted, `*out` (when given) is the tip read.
 */
SignedReceipt ReceivePeerAnnounceTipJson(std::string_view json, const std::vector<uint8_t>& mldsa_public_key,
                                         PeerAnnounceTip* out = nullptr);
/** `ReceivePeerAnnounceTip` as a result: update needed is `AppError::NewerFormat`, every other refusal an Error. */
Roe<void> VerifyPeerAnnounceTip(const PeerAnnounceTip& tip, const std::vector<uint8_t>& mldsa_public_key);

int64_t PeerAnnounceNextHeartbeatAtMs(int64_t last_emit_ms, double jitter_unit);
bool PeerAnnounceHeartbeatDue(int64_t last_emit_ms, int64_t now_ms);

} // namespace pbr
