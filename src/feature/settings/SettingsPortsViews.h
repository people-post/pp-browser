#pragma once

#include <string>

namespace pbr {

/** PIN vault readiness for Me → Security (app → ProfileSecretsService). */
struct PinProtectionView {
  bool ready = false;    // secrets initialized + has vault
  bool unlocked = false;
};

/**
 * Reachability card projection for Me → Network.
 * App maps MessagingHub::Reachability(); settings must not include libp2p.
 */
struct SettingsReachabilityView {
  enum class Status {
    Unknown,
    Checking,
    Reachable,
    OutboundOnly,
    Blocked,
  };

  Status status = Status::Unknown;
  bool has_global_ipv6 = false;
  bool dial_back_ok = false;
  bool upnp_mapped = false;
  /** From ReachabilityHelpKey — empty when no sheet. */
  std::string help_kind;
};

} // namespace pbr
