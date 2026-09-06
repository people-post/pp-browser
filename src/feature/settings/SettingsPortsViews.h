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
 * App maps ConversationsHub::Reachability(); settings must not include libp2p.
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


/** Me → Storage CAS library row (P3 / C013). */
struct CasLibraryItemView {
  std::string content_id_hex;
  std::string title;
  std::string detail;
  std::string realm_label;
  std::string pin_label;
  bool can_share_publicly = false;
  bool can_unpublish = false;
};

} // namespace pbr
