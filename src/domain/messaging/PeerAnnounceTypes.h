#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pbr {

inline constexpr int kPeerAnnounceTipSchemaVersion = 1;
inline constexpr const char* kPeerAnnounceAppNs = "pp-browser/peer-announce/1";

/** Live heartbeat: floor ≥30s, prefer jitter up to 60s (DESIGN). */
inline constexpr int64_t kPeerAnnounceLiveHeartbeatMinIntervalMs = 30'000;
inline constexpr int64_t kPeerAnnounceLiveHeartbeatMaxIntervalMs = 60'000;

enum class PeerAnnounceHelperCaps : uint32_t {
  None = 0,
  HelpAnnounce = 1u << 0,
  HelpMedia = 1u << 1,
};

inline constexpr PeerAnnounceHelperCaps operator|(PeerAnnounceHelperCaps a, PeerAnnounceHelperCaps b) {
  return static_cast<PeerAnnounceHelperCaps>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline constexpr bool HasPeerAnnounceHelperCap(PeerAnnounceHelperCaps set, PeerAnnounceHelperCaps bit) {
  return (static_cast<uint32_t>(set) & static_cast<uint32_t>(bit)) != 0;
}

enum class PeerAnnounceState : uint8_t { Scheduled = 0, Live = 1, Ended = 2 };

inline const char* PeerAnnounceStateToString(PeerAnnounceState state) {
  switch (state) {
  case PeerAnnounceState::Scheduled:
    return "scheduled";
  case PeerAnnounceState::Live:
    return "live";
  case PeerAnnounceState::Ended:
    return "ended";
  }
  return "scheduled";
}

inline std::optional<PeerAnnounceState> PeerAnnounceStateFromString(std::string_view value) {
  if (value == "scheduled") {
    return PeerAnnounceState::Scheduled;
  }
  if (value == "live") {
    return PeerAnnounceState::Live;
  }
  if (value == "ended") {
    return PeerAnnounceState::Ended;
  }
  return std::nullopt;
}

/** Signed tip under a PeerId-owned topic (Spine B). */
struct PeerAnnounceTip {
  int schema_version = kPeerAnnounceTipSchemaVersion;
  std::string peer_id;
  std::string topic_id;
  std::string program_id;
  PeerAnnounceState state = PeerAnnounceState::Scheduled;
  uint64_t seq = 0;
  uint64_t epoch = 0;
  int64_t created_at_ms = 0;
  std::string join_handle;
  std::string body;
  std::string content_id_hex;
  std::string signature_b64;
};

} // namespace pbr
