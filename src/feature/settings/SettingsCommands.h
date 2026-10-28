#pragma once

#include "foundation/diagnostics/DiagnosticsUploadResult.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/people/ProfileIdentityView.h"
#include "foundation/data/SessionStore.h"
#include "lib/net/BlobQuota.h"
#include "lib/net/ClientCompat.h"
#include "common/Error.h"
#include "feature/settings/SettingsPortsViews.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

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
  /** CAS library (P3): filter all|private|public|cache. */
  std::function<Roe<std::vector<CasLibraryItemView>>(std::string filter)> list_cas_library;
  std::function<Roe<void>(const std::string& private_content_id_hex)> share_cas_publicly;
  std::function<Roe<void>(const std::string& public_content_id_hex)> unpublish_cas;
  /** P4: fetch public tip from peer_relay_user_id into public Cache. */
  /** Asynchronous (a mesh fetch): `on_done` runs on UI. */
  std::function<void(const std::string& tip, const std::string& peer_relay_user_id,
                     std::function<void(Roe<void>)> on_done)>
      fetch_cas_public_tip;
  std::function<Roe<void>(const RegisterIdentityArgs& args)> register_identity;
  std::function<Roe<void>()> rotate_brief_llm_key;
  std::function<Roe<void>(int older_than_days)> clear_undelivered_older_than;
  std::function<void(bool try_upnp)> run_reachability_probe;
  std::function<void()> try_upnp_port_mapping;
  /** Wipe profile data and reinit hub/secrets — app owns that lifecycle. */
  std::function<Roe<void>()> reset_active_profile;
  /** Wipe the profile's chat data only (account, keys, contacts, settings stay) — app owns that lifecycle. */
  std::function<Roe<void>()> reset_chat_data;
  /** Display label for a language pref (`system` or BCP-47 tag). App → LocalizationService. */
  std::function<std::string(const std::string& language_pref)> language_display_label;
  std::function<std::vector<LocaleInfo>()> available_locales;
  /** Live-apply appearance (`system` / `light` / `dark`). App → Theme. */
  std::function<void(const std::string& appearance_pref)> apply_appearance;

  /** Live SessionStore for section Flush / Reset (app owns lifetime). */
  std::function<SessionStore&()> session_store;
  std::function<Roe<void>()> reload_from_disk;

  /** Messaging status without holding ConversationsHub*. */
  std::function<bool()> messaging_ready;
  /** Amp mesh attached (async after messaging_ready). Node connection card uses this. */
  std::function<bool()> mesh_ready;
  std::function<std::string()> last_mesh_error;
  /** Runtime Amp listen multiaddr (empty when mesh is down). */
  std::function<std::string()> amp_listen_multiaddr;
  std::function<SettingsReachabilityView()> load_reachability;
  /** Refresh Me / Network attention dots after reachability nudge ack. */
  std::function<void()> refresh_nav_badges;

  std::function<PinProtectionView()> load_pin_protection;
  /** Change the profile PIN via the app-owned vault (caller ensures unlocked first). */
  std::function<Roe<void>(const std::string& current_pin, const std::string& new_pin)> change_pin;
  /** Copy a time-limited link-device payload (account keys, shared DEK, account card; no sessions — M021). */
  std::function<Roe<std::string>()> export_link_device;
  /** The recovery phrase once `pin` opens the vault (M023); nullopt for an account without one. */
  std::function<Roe<std::optional<std::string>>(const std::string& pin)> recovery_phrase;
  /** Whether the account has a recovery phrase (vault unlocked). */
  std::function<bool()> has_recovery_phrase;
  /** The account's state as a saved copy's text (an exported locker file, M023 §6 amendment). */
  std::function<Roe<std::string>()> export_account_copy;
  /** The account's devices (M021 d4): the main device first; whether this one is signed out. */
  std::function<Roe<OwnDevicesView>()> own_devices;
  /** On the main device: take another device off the account (it stops working until linked again). */
  std::function<Roe<void>(const std::string& peer_id)> remove_own_device;
  /** On the main device: make another device the main one (this one can then no longer add or remove devices). */
  std::function<Roe<void>(const std::string& peer_id)> make_main_device;

  /** Current app Support discovery (enabled + account_id); nullopt when off/unavailable. */
  std::function<std::optional<ClientCompatSupport>()> load_support_discovery;
  /** Ensure PP Support contact + open Direct e2e_public chat; `prefilled_text` (may be empty) goes in the composer, unsent. */
  std::function<Roe<void>(const std::string& prefilled_text)> open_support_chat;
  /** Send diagnostics (Security): bundles and uploads on a worker; `on_done` runs on UI. One at a time. */
  std::function<void(std::function<void(DiagnosticsUploadResult)> on_done)> send_diagnostics;
  /** Reference of the last successful send (`diagnostics/last_upload.json`); "" when none. */
  std::function<std::string()> last_diagnostics_id;
};

} // namespace pbr
