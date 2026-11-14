#pragma once

#include <string>
#include <vector>

namespace pbr {

/** PIN vault readiness for Me → Security (app → ProfileSecretsEngine). */
struct PinProtectionView {
  bool ready = false; // secrets initialized + has vault
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
  bool can_copy_tip = false;
};

/** Me → Security → Your devices: one of the account's devices (M021 d4), with the name it was given. */
struct OwnDeviceView {
  std::string peer_id;
  /** The device's name; "New device" when it has none (never a piece of its PeerId). */
  std::string name;
  /** The device has a name (the rename dialog starts from it; empty otherwise). */
  bool named = false;
  bool is_this_device = false;
  /** The account's main device: the first on the list, the only one that adds or removes devices. */
  bool is_main = false;
  /** This device may take it off the account, make it the main one or rename it (this is the main device; it is another). */
  bool can_manage = false;
};

/** The account's devices as this device knows them, in list order (the main device first). */
struct OwnDevicesView {
  std::vector<OwnDeviceView> devices;
  /** The account's newest list leaves this device out: it no longer works until linked again. */
  bool signed_out = false;
  /** This device is the main one (or the account has no list yet): it may add and remove devices. */
  bool this_is_main = false;
};

} // namespace pbr
