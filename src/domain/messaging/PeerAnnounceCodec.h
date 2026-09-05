#pragma once

#include "domain/messaging/PeerAnnounceTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

Roe<std::string> MakePeerAnnounceTopicId(std::string_view peer_id, std::string_view local_name,
                                         std::string_view app_ns = kPeerAnnounceAppNs);

std::string PeerAnnounceCanonicalSignBytes(const PeerAnnounceTip& tip);

Roe<std::string> EncodePeerAnnounceTipJson(const PeerAnnounceTip& tip);
Roe<PeerAnnounceTip> DecodePeerAnnounceTipJson(std::string_view json);

/** Sign with device ML-DSA-65 secret (PeerId-bound; matches IdentityStore device key). */
Roe<PeerAnnounceTip> SignPeerAnnounceTip(PeerAnnounceTip tip, const std::vector<uint8_t>& mldsa_secret_key);
Roe<void> VerifyPeerAnnounceTip(const PeerAnnounceTip& tip, const std::vector<uint8_t>& mldsa_public_key);

int64_t PeerAnnounceNextHeartbeatAtMs(int64_t last_emit_ms, double jitter_unit);
bool PeerAnnounceHeartbeatDue(int64_t last_emit_ms, int64_t now_ms);

} // namespace pbr
