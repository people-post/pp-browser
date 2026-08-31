#pragma once

#include "base/adp/Types.h"

#include <chrono>
#include <cstddef>
#include <string>

namespace pbr::amp {

inline constexpr const char* kAdpMultiaddrProtocol = "adp";
inline constexpr const char* kAdpMultiaddrVersion = "1.0.0";

/** Pre-MSH ADP HMAC key (documented constant; upgraded to K_assoc after handshake). */
adp::PeerKey PreSessionPeerKey();

/** Reserved assoc id for MSH handshake before K_assoc is derived. */
adp::AssocId PreSessionAssocId();

enum class PeerLinkPhase {
  Unavailable,
  Idle,
  Dialing,
  Handshaking,
  Connected,
  Backoff,
};

struct PeerLinkSnapshot {
  PeerLinkPhase phase = PeerLinkPhase::Unavailable;
  std::chrono::milliseconds backoff_remaining{0};
  std::string detail;
  bool has_endpoint = false;
};

struct PeerLinkConfig {
  size_t max_links = 48;
  size_t max_concurrent_dials = 6;
  std::chrono::milliseconds dial_timeout{8000};
  std::chrono::milliseconds idle_ttl{180000};
  std::chrono::milliseconds dial_failure_backoff{30000};
};

} // namespace pbr::amp
