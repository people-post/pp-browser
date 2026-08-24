#pragma once

#include "base/i18n/LocalizationService.h"
#include "base/people/ProfileIdentityView.h"
#include "base/data/SessionStore.h"
#include "base/net/BlobQuotaUtil.h"
#include "common/Error.h"
#include "feature/settings/SettingsPortsViews.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

/** Input for register / renew — not the full Me form bag. */
struct RegisterIdentityArgs {
  /** Empty = leave the stored identity nickname unchanged. */
  std::string nickname;
};

/**
 * Settings ports. Declared here (consumer); Application fills implementations.
 * Not a singleton. No BindMessaging / held service pointers — clear via BindCommands({}).
 */
struct SettingsCommands {
  std::function<ProfileIdentityView()> load_profile_identity;
  std::function<Roe<void>(const std::string& nickname)> save_profile_nickname;
  std::function<void(std::function<void(std::vector<std::string> paths)> on_picked)> pick_profile_icon_image;
  std::function<Roe<void>(const std::string& path)> upload_profile_icon_file;
  std::function<Roe<void>()> clear_profile_icon;
  std::function<Roe<BlobQuotaRecoveryPlan>()> plan_relay_quota_recovery;
  std::function<Roe<void>()> free_oldest_relay_blob_slot;
  std::function<void()> drain_pending_attachment_media;
  std::function<Roe<void>()> clear_downloaded_attachments;
  std::function<Roe<void>(const RegisterIdentityArgs& args)> register_identity;
  std::function<Roe<void>()> rotate_brief_llm_key;
  std::function<Roe<void>(int older_than_days)> clear_undelivered_older_than;
  std::function<void(bool try_upnp)> run_reachability_probe;
  std::function<void()> try_upnp_port_mapping;
  /** Wipe profile data and reinit hub/secrets — app owns that lifecycle. */
  std::function<Roe<void>()> reset_active_profile;
  /** Display label for a language pref (`system` or BCP-47 tag). App → LocalizationService. */
  std::function<std::string(const std::string& language_pref)> language_display_label;
  std::function<std::vector<LocaleInfo>()> available_locales;
  /** Live-apply appearance (`system` / `light` / `dark`). App → Theme. */
  std::function<void(const std::string& appearance_pref)> apply_appearance;

  /** Live SessionStore for section Flush / Reset (app owns lifetime). */
  std::function<SessionStore&()> session_store;
  std::function<Roe<void>()> reload_from_disk;

  /** Messaging status without holding MessagingHub*. */
  std::function<bool()> messaging_ready;
  std::function<std::string()> last_libp2p_error;
  std::function<SettingsReachabilityView()> load_reachability;
  /** Refresh Me / Network attention dots after reachability nudge ack. */
  std::function<void()> refresh_nav_badges;

  std::function<PinProtectionView()> load_pin_protection;
  /** Change the profile PIN via the app-owned vault (caller ensures unlocked first). */
  std::function<Roe<void>(const std::string& current_pin, const std::string& new_pin)> change_pin;
  /** Copy a time-limited link-device payload (account keys + shared DEK + public PSKs). */
  std::function<Roe<std::string>()> export_link_device;
};

} // namespace pbr
