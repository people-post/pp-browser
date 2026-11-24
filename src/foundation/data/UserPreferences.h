#pragma once

#include "foundation/data/ToolPermissions.h"
#include "common/Error.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

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
  WindowPrefs window;
  SafeAreaInsets safe_area;
  DisplayPrefs display;
  /**
   * Settings → Advanced → "Collect run logs" (DEV/PLAN-send-run-logs.md): on keeps 7 days / 50 MB of logs, off only
   * this launch's and the one before. Device-wide like the log directory (every profile and backend shares it),
   * hence here and not in a profile's preferences. Optional on disk (absent = off); the schema version stays.
   */
  bool collect_run_logs = false;
  /**
   * Settings → Advanced → "Collect crash logs": when on, Send run logs also carries the crash records. Device-wide,
   * optional on disk (absent = off). Not the old per-profile `crash_reports_enabled` (its upload stays dormant).
   */
  bool collect_crash_logs = false;
  /**
   * The Chats page's "new version" card (DEV/PLAN-release1-update-and-support.md A1): which latest_client_version the
   * count belongs to, how often "Later" was tapped on it, and when last. Device-wide (an update has nothing to do
   * with the account), optional on disk (absent = never tapped); the schema version stays, an older build ignores them.
   */
  std::string update_card_version;
  int update_card_dismissals = 0;
  int64_t update_card_dismissed_ms = 0;
};

struct ProfilePreferences {
  static constexpr int kSchemaVersion = 16;

  int schema_version = kSchemaVersion;
  std::string theme = "themes/base.rcss";
  std::string appearance = "system";
  /** UI language: `system` or a shipped BCP-47 tag (`en`, `zh-Hans`). */
  std::string language = "system";
  /** True when vault was created with kDefaultProfilePin and not yet changed. */
  bool pin_is_default = false;
  /**
   * The PIN's kind, not a secret (DEV/PLAN-pin-gate-redesign.md B): nullopt = unknown (a profile from before the
   * field, until its next PIN unlock), 0 = a text PIN, 4–8 = a digit PIN of that many digits (the phone's keypad).
   * Optional on disk; the schema version stays.
   */
  std::optional<int> pin_digits;
  /** The user has opened their recovery phrase on Me → Security once (Home's Security widget; device-only). */
  bool recovery_phrase_seen = false;
  /** The Chats page's "save your recovery phrase" card: how often it was put off (0-2; it comes back once after 7 days) and when last. Device-only. */
  int recovery_reminder_dismissals = 0;
  int64_t recovery_reminder_dismissed_ms = 0;
  /** Phone chat composer: the last input mode was voice (true) rather than keyboard (false). Optional on disk; device-only, never synced. */
  bool composer_voice_mode_last = false;
  /**
   * An account was brought over (linking code or recovery phrase) and its first account-data sync has not finished:
   * the Contacts and Chats pages show "Syncing…" instead of the empty state. Set when the import ends, cleared when
   * the first locker sync succeeds. Optional on disk (absent = false = not pending; no schema bump); device-only,
   * never synced.
   */
  bool account_sync_pending = false;
  /**
   * PIN grace window (#21): minutes after the last foreground use during which a cold start or call skips the PIN.
   * One of 0 (every time), 5, 15, 60, or -1 (kUnlockGraceAlways: until the phone restarts, A015); anything else reads back as 15. Device-only, never synced.
   */
  int unlock_grace_minutes = 15;
  /** When true, renew network registration near/past expiry after unlock. */
  bool auto_renew_registration = true;
  /** P005 (amended by P010) — message banners; push stays registered and calls ring either way. */
  bool show_notifications = true;
  /** Banners show the message text; off shows a neutral line instead (sender or group stays in the title). */
  bool show_notification_preview = true;
  /**
   * P011 — Android: stay connected in the background (a foreground service keeps the app working,
   * the relay polled slowly) so calls and messages arrive without push. Opt-in: costs battery.
   */
  bool stay_connected = false;
  /**
   * V032 — show call media diagnostics (debug subtitle + rich Call details).
   * OR'd with CLI `--debug` via CallDiagnosticsEnabled().
   */
  bool call_diagnostics = false;
  /** When true, allow uploading local crash dumps on next launch (opt-in). */
  bool crash_reports_enabled = false;
  /** G007 — inbound group invite policy: everyone | contacts_only | nobody */
  std::string group_invite_policy = "everyone";
  /** projects/privacy T3 — who may call this profile: everyone | contacts_only | nobody. */
  std::string call_invite_policy = "everyone";
  /** R021 — attachment download: smart | always_auto | on_demand */
  std::string attachment_download_policy = "smart";
  /** With always_auto: download everything on Wi-Fi / on the mobile network. Neither ticked reads back as smart. */
  bool attachment_auto_on_wifi = true;
  bool attachment_auto_on_cellular = false;
  /** Me → About: show the technical options (ids, endpoints, MCP, paths). Off for everyone by default. */
  bool show_advanced_settings = false;
  /** Me → Help, five taps on the version: the internal test options (and the Staging / Dev services in the service list). Off by default. */
  bool show_tester_options = false;
  /** First-run wizard finished or skipped (or an account was brought over). */
  bool onboarding_completed = false;
  /** The ask-AI panel's one-time privacy card was dismissed ("Got it"; DEV/PLAN-ui-batch5-ask-ai.md 5a). */
  bool ai_panel_intro_seen = false;
  /** Phone AI dot (DESIGN-pp-ui §8.14): Me → AI assistant → Show AI dot. */
  bool ai_dot_shown = true;
  /**
   * Its look, also the AI Home's mascot's: `auto` (default; a different look each weekday, gui/shell/MascotLook.h), or
   * a fixed one: `a` one eye, `b` two eyes, `c` cool, `d` plain, `e` Friday, `f` Saturday, `g` Sunday.
   */
  std::string ai_dot_look = "auto";
  /** Its size: `s` 52dp, `m` 64dp (default), `l` 76dp (Me → AI assistant → Assistant size). */
  std::string ai_dot_size = "m";
  /** Where it rests: the left or right edge, and how far down its band (0 top – 1000 bottom). */
  bool ai_dot_left = false;
  int ai_dot_y_permille = 600;
  /** Tucked half off that edge (flung or dragged into it); stays tucked until dragged out. */
  bool ai_dot_tucked = false;
  /** When true, compact shell chrome uses opaque surfaces only (no backdrop frost). */
  bool reduce_transparency = false;
  /** When false, disables the single-surface frost tier (dogfood / perf); opaque chrome remains. */
  bool compact_chrome_frost = true;
  /** Expanded layout: secondary pane (list) width in dp; shell clamps to ShellLayout bounds. */
  int sidebar_width_dp = 240;
  /** Expanded layout: secondary pane (list) hidden by the user. */
  bool sidebar_collapsed = false;
  /** Expanded layout: auxiliary (right-hand) pane width in dp; shell clamps to ShellLayout bounds. */
  int auxiliary_width_dp = 320;
  /**
   * Last acked Me → Network reachability nudge (`outbound_only` / `blocked`, or empty).
   * Cleared when status becomes reachable so a later regression can nudge again.
   */
  std::string reachability_nudge_acked_status;
  /** Agent tool trust: ask / allow / deny by tool or provider (schema v11). */
  ToolPermissionsPrefs tool_permissions;
  /** MRU emoji glyphs for the in-app picker (schema v12; cap enforced by EmojiCatalog). */
  std::vector<std::string> recent_emojis;
  /**
   * Phone Home widgets the user switched off in Edit Home (`messages`, `new_people`, `brief`); empty shows all.
   * Device-only, like thread_local_prefs: never synced.
   */
  std::vector<std::string> home_widgets_hidden;
  /** Their order on Home (Edit Home's drag handles); empty: the default order. Device-only, never synced. */
  std::vector<std::string> home_widgets_order;
  /** Phone Home shows the 动态 widgets section; default off = minimal AI-first Home. Device-only, never synced. */
  bool home_show_dynamic = false;
  /** The AI Home's logo is the animated mascot (Settings → Appearance → Animated home logo); off: the static icon. */
  bool home_animated_logo = true;
  /**
   * Contacts starred as favourites ("常用联系人"; contact ids, in the order they were starred): first in Home's
   * Favourites widget and the New chat page's frequent group. Device-only, like thread_local_prefs: never synced.
   */
  std::vector<std::string> starred_contacts;
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
