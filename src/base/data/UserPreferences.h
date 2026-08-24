#pragma once

#include "base/data/ToolPermissions.h"
#include "common/Error.h"

#include <string>
#include <vector>

namespace pbr {

struct SafeAreaInsets {
  int top = 0;
  int bottom = 0;
  int left = 0;
  int right = 0;
};

struct WindowPrefs {
  int width = 1280;
  int height = 720;
};

struct DisplayPrefs {
  bool fullscreen = false;
};

struct MachinePreferences {
  static constexpr int kSchemaVersion = 1;

  int schema_version = kSchemaVersion;
  std::string active_profile_id = "default";
  WindowPrefs window;
  SafeAreaInsets safe_area;
  DisplayPrefs display;
};

struct ProfilePreferences {
  static constexpr int kSchemaVersion = 13;

  int schema_version = kSchemaVersion;
  std::string theme = "themes/base.rcss";
  std::string appearance = "system";
  /** UI language: `system` or a shipped BCP-47 tag (`en`, `zh-Hans`). */
  std::string language = "system";
  /** True when vault was created with kDefaultProfilePin and not yet changed. */
  bool pin_is_default = false;
  /** When true, renew network registration near/past expiry after unlock. */
  bool auto_renew_registration = true;
  /** P005 — OS banners + FCM registration; sync continues when false. */
  bool show_notifications = true;
  /**
   * V032 — show call media diagnostics (debug subtitle + rich Call details).
   * OR'd with CLI `--debug` via CallDiagnosticsEnabled().
   */
  bool call_diagnostics = false;
  /** G007 — inbound group invite policy: everyone | contacts_only | nobody */
  std::string group_invite_policy = "contacts_only";
  /** R021 — attachment download: smart | always_auto | on_demand */
  std::string attachment_download_policy = "smart";
  /** When true, compact shell chrome uses opaque surfaces only (no backdrop frost). */
  bool reduce_transparency = false;
  /** When false, disables the single-surface frost tier (dogfood / perf); opaque chrome remains. */
  bool compact_chrome_frost = true;
  /**
   * Last acked Me → Network reachability nudge (`outbound_only` / `blocked`, or empty).
   * Cleared when status becomes reachable so a later regression can nudge again.
   */
  std::string reachability_nudge_acked_status;
  /** Agent tool trust: ask / allow / deny by tool or provider (schema v11). */
  ToolPermissionsPrefs tool_permissions;
  /** MRU emoji glyphs for the in-app picker (schema v12; cap enforced by EmojiCatalog). */
  std::vector<std::string> recent_emojis;
};

class UserPreferences {
public:
  static Roe<MachinePreferences> LoadMachine(const std::string& data_dir);
  static Roe<void> SaveMachine(const std::string& data_dir, const MachinePreferences& prefs);

  static Roe<ProfilePreferences> LoadProfile(const std::string& profile_data_dir);
  static Roe<void> SaveProfile(const std::string& profile_data_dir, const ProfilePreferences& prefs);

  static MachinePreferences DefaultMachine();
  static ProfilePreferences DefaultProfile();
};

} // namespace pbr
