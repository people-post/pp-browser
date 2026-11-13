#pragma once

#include "lib/ui/ShellTypes.h"
#include "foundation/data/UserPreferences.h"
#include "common/Module.h"
#include "gui/shell/ShellAiDot.h"
#include "gui/shell/ShellHomeMascot.h"
#include "gui/shell/ShellLayoutRml.h"
#include "gui/shell/ShellStatusbar.h"
#include "gui/shell/StatusbarText.h"
#include "gui/CallActionsPorts.h"
#include "gui/CallChromeSync.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/PinGateActionPorts.h"
#include "gui/shell/ShellCallChromePorts.h"
#include "gui/shell/ShellCallChromeGesture.h"
#include "gui/shell/ShellDrawerOpenGesture.h"
#include "lib/ui/ShellGestureAxis.h"
#include "gui/shell/ShellLargeTitleScroll.h"
#include "gui/shell/ShellFavoritesSnap.h"
#include "gui/shell/ShellNavBarScroll.h"
#include "gui/shell/ShellSplitterDrag.h"
#include "gui/shell/SessionSwipeGesture.h"
#include "gui/shell/ShellPageSheetGesture.h"
#include "gui/shell/ShellSwipeBackGesture.h"

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Event.h>
#include <ui/base/Types.h>

#include <chrono>
#include <functional>
#include <optional>
#include <source_location>
#include <map>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace ui {
class Context;
class Element;
} // namespace ui

namespace pbr {

enum class DismissStyle { Instant, Animated };

enum class DismissTarget {
  None,
  LocalBack,
  CompactChatOverlay,
  AuxiliarySheet,
  Transient,
  OverlayLayer,
  /** Phone Me / Contacts root page: back to the tab it was entered from (ShellHost::HiddenTabBack). */
  HiddenTab,
};

struct LocalBackEntry {
  std::string id;
  std::function<void()> commit;
};

class ShellHost : public Module {
public:
  /** Theme / appearance / chrome materials projected from ProfilePreferences. */
  struct ChromePrefs {
    std::string theme = "themes/base.rcss";
    std::string appearance = "system";
    bool reduce_transparency = false;
    bool compact_chrome_frost = true;
    /** Phone AI dot: shown or not, its look (`a`–`g`) and size (`s` / `m` / `l`). */
    bool ai_dot = true;
    std::string ai_dot_look = "auto";
    std::string ai_dot_size = "m";
    /** Me -> AI assistant -> Animated home logo. */
    bool home_animated_logo = true;

    bool operator==(const ChromePrefs& other) const {
      return theme == other.theme && appearance == other.appearance &&
             reduce_transparency == other.reduce_transparency && compact_chrome_frost == other.compact_chrome_frost &&
             ai_dot == other.ai_dot && ai_dot_look == other.ai_dot_look && ai_dot_size == other.ai_dot_size &&
             home_animated_logo == other.home_animated_logo;
    }
    bool operator!=(const ChromePrefs& other) const { return !(*this == other); }
  };

  /** Whether going from `previous` to `next` needs ShellHost::Apply (the material, the dot, the home logo). */
  static bool NeedsApply(const ChromePrefs& next, const ChromePrefs* previous) {
    return !previous || next.reduce_transparency != previous->reduce_transparency ||
           next.compact_chrome_frost != previous->compact_chrome_frost || next.ai_dot != previous->ai_dot ||
           next.ai_dot_look != previous->ai_dot_look || next.ai_dot_size != previous->ai_dot_size ||
           next.home_animated_logo != previous->home_animated_logo;
  }

  static ChromePrefs ProjectChrome(const ProfilePreferences& prefs);
  /** Apply material fields only (theme/appearance are Theme-owned). */
  void Apply(const ChromePrefs& prefs);

  ShellHost();

  void BindShellMessaging(MessagingShellPorts ports);
  /** Advanced settings shown or not (the status popover's network-settings link follows it). */
  void SetShowAdvancedSettings(bool shown);
  void BindPinGateActions(PinGateActionPorts ports);
  void BindFlowCoordinator(FlowCoordinatorPorts ports);
  void BindCallActions(CallActionsPorts ports);
  void BindScanAction(std::function<void()> action);

  void Initialize(ui::Context* context);
  void SyncLayout();
  /**
   * `reason` names the caller for the log and the shell.sync_layout_slow metric. Without one, the call
   * site (file:line) is used, so every request can be told apart.
   */
  void RequestSyncLayout(bool restore_focus_after = false, const char* reason = nullptr,
                         std::source_location where = std::source_location::current());
  /** Mount/clear call ring + in-call overlays without remounting the full shell tree.
   *  Defers to the next UI turn so Rml click handlers are not mid-dispatch on destroyed nodes. */
  void RemountCallChrome();
  /** Mount/clear alert/confirm/prompt into #shell-dialog-mount (not full SyncLayout). */
  void RemountDialogChrome();
  /** Mount/clear PIN gate into #shell-pin-gate-mount (not full SyncLayout). */
  void RemountPinGateChrome();
  /** Mount `body` into the overlay mount element `mount_id` (empty body clears). Deferred to the next UI turn. */
  void MountOverlayBody(const std::string& mount_id, const std::string& body);
  void Update(ui::Context* context);
  /** Call after ui::Context::Update so RequestNextUpdate is not cleared by it. Arms power-save. */
  void NotifyFrameEnd(ui::Context* context);

  ShellState& State() { return state_; }
  const ShellState& State() const { return state_; }

  void RegisterPane(const PaneSpec& spec);
  void SetAuxiliaryAvailable(bool available);
  void OpenAuxiliary();
  void CloseAuxiliary();
  void ToggleAuxiliary();
  /**
   * On phones, moving from a tab in the bar to a hidden tab (ShellLayout::IsHiddenTabOnCompact: Me, Contacts) by any
   * path remembers the tab left, for HiddenTabBack; a chat overlay open there stays open under the hidden tab's page.
   */
  void SelectNavTab(NavTab tab);
  /** A tab-bar click (the select_nav_tab event); a second tap on the AI tab stays put. */
  void OnNavTabClicked(const std::string& tab_name);
  /** Phone hidden tab's root page (its back, edge swipe, system back): the tab it was entered from (else Chats). */
  void HiddenTabBack();
  /** Phone Settings: a secret page (Move to a new device) is shown over a section; the tab bar is off while it is. */
  void SetSecretPageOpen(bool open);
  /** An avatar opens Settings (the Me tab) from anywhere. */
  void OpenSettingsFromAvatar();
  /**
   * Phone AI page: the AI conversation list slides in from the left (DEV/PLAN-minimal-nav.md C). Opening first lets
   * go of the composer (keyboard down, its inset back to the home indicator's). No-op on the desktop. The drawer is
   * closed again by its ✕ / scrim, a picked conversation (OpenCompactChat), a tab switch, the desktop layout, a call
   * screen or ring, a pushed page or layer, and the system back (HandleDismiss, before anything under it). Closing
   * lets go of the drawer's row swipes at once; its DOM goes on the next UI turn (RemountAiDrawer), never inside the
   * click that closed it.
   */
  void OpenAiDrawer();
  void CloseAiDrawer();
  /** The drawer's rows have their swipe listeners (only while it is mounted). */
  bool AiDrawerRowSwipesAttached() const { return drawer_session_swipe_.Attached(); }
  /** Phone layout: the AI page (Home) shows with nothing over it (no chat, pushed page, layer or drawer). */
  bool HomeRootShown() const;
  /** Phone layout: the hidden tab's own root page is on top (nothing pushed over it, no settings page open). */
  bool HiddenTabRootShown() const;
  /** What a back (edge swipe, system back, the arrow) closes now: the one resolver they all go through. */
  DismissTarget BackTarget() const { return ResolveDismissTarget(); }
  /** Phone Settings home with no chat thread under it: a root page of the tab bar (no back arrow, no edge swipe). */
  bool SettingsHomeRootShown() const;
  /** Phone root pages: flips the large-title state; dirties the model only when it changes. */
  void SetLargeTitleCollapsed(bool collapsed);
  /** Chats header status line, event-driven (never polled): known=false shows nothing; else online/offline. Dirties only on change. */
  void SetChatsLink(ChatsLink link);
  void SetChatsLink(const bool known, const bool online) {
    SetChatsLink(!known ? ChatsLink::Unknown : (online ? ChatsLink::Online : ChatsLink::Offline));
  }
  /** `keep_compact_chat`: a chat overlay stays open (under a hidden tab, or again on the way back to it). */
  void ClearTabContext(bool keep_compact_chat = false);
  void SetPrimaryPane(const std::string& key);
  void ClearPrimaryPane();
  void OpenCompactChat();
  void CloseCompactChat();
  void PushTransient(const PaneSpec& spec);
  void PopTransient();
  int PushLayer(const PaneSpec& spec);
  void CloseLayer(int layer_id = -1);
  /** Layer `layer_id` (PushLayer) is still open: not closed by its X, the scrim, a swipe or code. */
  bool HasLayer(int layer_id) const;
  /** Closes every page sheet layer, top first (each owner hears its own close as for a swipe). */
  void ClosePageSheets();

  /**
   * Single dismiss entry for Escape, back buttons, and swipe gestures.
   * Animated style schedules CommitDismiss after the exit transition.
   * When `force` is set, that target is dismissed (sheet swipe ignores local back).
   */
  bool RequestDismiss(DismissStyle style = DismissStyle::Instant, DismissTarget force = DismissTarget::None);
  bool HandleDismiss();

  /** Nested back within the current interruption (e.g. settings list→detail). */
  void PushLocalBack(const std::string& id, std::function<void()> commit);
  bool ClearLocalBack(const std::string& id);
  bool HasLocalBack(const std::string& id) const;
  void RefreshDismissGestures();

  /** Full window-model refresh (e.g. after SyncLayout remount). Prefer domain helpers. */
  void DirtyWindow();
  /** Nav / layout / sheet / auxiliary / transient binding keys. */
  void DirtyNavChrome();
  /** Banner / toast / dialog binding keys. */
  void DirtyFeedback();
  /** PIN gate + unlock_in_progress binding keys. */
  void DirtyPinGate();
  /** Activity / statusbar / titlebar / fonts_ready binding keys. */
  /** Call ring / in-progress window model keys (not nav/dialog/pin). */
  void DirtyCallChrome();
  /**
   * Copy presenter call-chrome snapshot into State, then Remount or DirtyCallChrome + force-frame.
   * Classification (None / DirtyOnly / Remount) is owned by CallController.
   */
  void ApplyCallChromeSnapshot(const CallChromeSnapshot& snapshot, CallChromeUpdate update);
  /** The call page's content height in dp (window less the status bar and the bottom inset); 0 before a window exists. */
  int CallPageHeightDp() const;
  /** Copy presenter PIN gate snapshot into State (binding target). */
  void ApplyPinGateState(const PinGateState& state);
  /**
   * Recompute startup_cover_visible from armed + unlock_in_progress + pin gate.
   * PIN input always wins (cover hides).
   */
  void ReconcileStartupCover();
  /** Clear the first-paint arm after deferred unlock has decided what to do. */
  void SettleStartupCoverArm();
  // Deferred remount of the nav rail (safe from click handlers). Use when badge counts change
  // and DirtyVariable alone may not refresh data-if views.
  void RequestRemountNavRail();
  void SetActivityVisible(bool visible);
  /** Busy indicator: top strip (compact/mobile) and status-bar activity text (desktop expanded). */
  void SetActivity(bool visible, const ui::String& message = {});
  void SetOnBeforeTransientMount(std::function<void(const std::string& key)> callback);
  void SetOnTransientMounted(std::function<void(const std::string& key)> callback);
  void SetOnTransientPopped(std::function<void(const std::string& key)> callback);
  void SetOnNavTabChanged(std::function<void(NavTab tab)> callback);
  /**
   * Phone: back from a hidden tab (Me, Contacts) to the tab it covered, where a chat overlay stayed open: the chat is
   * on screen again (after the tab-changed callback). Its messages are read now (ChatController).
   */
  void SetOnCompactChatShownAgain(std::function<void()> callback);
  void SetOnLayoutModeChanged(std::function<void(LayoutMode mode)> callback);

  /** Secondary-pane prefs from disk (width is clamped). Remounts when they differ from the current state. */
  void SetSidebarPrefs(int width_dp, bool collapsed);
  /** Fired after the user resizes or collapses/expands the secondary pane (persist here). */
  void SetOnSidebarChanged(std::function<void(int width_dp, bool collapsed)> callback);
  /** Auxiliary-pane width from disk (clamped). Remounts when it differs from the current state. */
  void SetAuxiliaryWidthPref(int width_dp);
  /** Fired after the user resizes the auxiliary pane (persist here). */
  void SetOnAuxiliaryWidthChanged(std::function<void(int width_dp)> callback);
  /** Collapse/expand the secondary pane (expanded layout, tabs with a secondary pane only). */
  void ToggleSidebarCollapsed();
  void SetOnLayoutSynced(std::function<void()> callback);
  /** Called as each SyncLayout ends with its whole duration in ms (measurement: chat.open's layout_ms). */
  void SetOnSyncLayoutMeasured(std::function<void(int64_t ms)> callback);
  /** Phone Chats list: a swipe-row button (or a full swipe) was used on `thread_id`. */
  void SetOnSessionSwipeAction(SessionSwipeGesture::ActionCallback callback);
  /** Phone New chat page: a swipe-row button (`swipe-action` text as in new_chat.rml) was used on `contact_id`. */
  void SetOnNewChatSwipeAction(SessionSwipeGesture::NamedActionCallback callback);

  /** Seed safe-area insets from machine.json (used when SDL reports zero). */
  void SetSafeAreaInsetsFromPrefs(int top_dp, int bottom_dp);
  void RefreshSafeAreaInsets(ui::Context* context);

  /**
   * Mobile/compact IME-replacement bottom panel (no scrim, remount-only).
   * Dismisses the OSK and latches height from the last IME inset (or a default).
   * @return false when bottom-chrome presentation is not used (expanded desktop).
   */
  bool SetBottomChrome(const BottomChromeSpec& spec);
  void ClearBottomChrome();
  bool BottomChromeOpen() const { return bottom_chrome_open_; }
  bool UsesBottomChromePresentation() const;
  void SetOnBottomChromeDismissed(std::function<void()> callback);

  /** Sync compact chrome material prefs from profile; resyncs shell when changed. */
  void SyncChromeMaterialPrefs(bool reduce_transparency, bool compact_chrome_frost);

  /**
   * Phone AI dot (DESIGN-pp-ui §8.14): the Me → AI assistant switch, look (`a`–`d`) and size (`s` / `m` / `l`); takes
   * effect at once.
   */
  /** `look` is the pref: `auto` (the day's look, LookForDate) or `a`-`d`. */
  void SetAiDot(bool enabled, const std::string& look, const std::string& size);
  /** Settings -> Appearance -> Animated home logo (the AI Home's logo is the mascot, or the static icon). */
  void SetHomeAnimatedLogo(bool animated);
  /** Where the AI dot rests, from disk (side, height in its band 0–1000). */
  void SetAiDotPlace(bool left, int y_permille, bool tucked);
  /** A tap on the AI dot (open the ask-AI panel). */
  void SetOnAiDotTap(std::function<void()> callback);
  /** The user dragged the AI dot to a new resting place (persist it). */
  void SetOnAiDotMoved(std::function<void(bool left, int y_permille, bool tucked)> callback);
  /**
   * True while a page with secrets on it is up (Me → Account & security: change PIN, recovery phrase). The AI dot does
   * not sit on it, and neither it nor the PIN gate is in the OS's copies of the screen (SecureScreen).
   */
  void SetSecretPage(std::function<bool()> showing);
  /** True while a secret can be read on screen (the recovery phrase shown): SecureScreen::SetSecretLegible. */
  void SetSecretLegible(std::function<bool()> showing);

  bool RegisterWindowModel(ui::Context* context);

  friend struct ShellHostTestAccess; // tests press the call answer handlers

private:
  // Data-model events with more to them than one call.
  void DismissBanner();
  void DialogOk();
  void DialogCancel();
  void DialogToggleCheckbox();
  void PinGateSubmit();
  void PinGatePaste(); // read the clipboard into the transfer-code / phrase box (iOS has no paste menu there, B163)
  void PinGateClearPayload(); // clear a pasted transfer code so the textarea comes back (B166)
  void PinGateCancel();
  void PinGateSetPin();
  void PinGateUseDefault();
  void PinGateIdentityNew();
  void PinGateIdentityLink();
  void PinGateIdentityPhrase();
  void PinGateUseSavedCopy();
  void PinGateForgotToggle();
  void PinGateUsePinInstead(); // "Enter PIN instead" while waiting for the phone's unlock (PIN grace window)
  void CallAccept();
  void CallAcceptVoice();
  void CallAcceptCharge();
  void CallDecline();
  void CallLeave();
  void CallRetry();
  void CallRedial();
  void CallMute();
  void CallCamera();
  void CallFlipCamera();
  void CallSpeaker();
  void CallInvite();
  void CallMinimize();
  void CallExpand();
  void CallImmersive();
  void CallRestore();
  void CallDetails();
  void TitlebarMinimize();
  void TitlebarToggleMaximize();
  void TitlebarClose();

  ui::Element* ShellRoot() const;
  /** The shell's markup from its state now. */
  ShellLayoutRml Rml() const;
  const char* NavContentKey() const;
  void MountPaneBodies();
  void RemountBottomChrome();
  void FlushRemountBottomChrome();
  void RemountBottomChromeNow();
  void MountNavRail();
  void MountNavContent();
  void MountComposer();
  void DetachDismissGestures();
  void AttachSwipeBackGesture();
  void AttachSplitterDrag();
  void AttachLargeTitleScroll();
  void AttachAuxiliarySplitterDrag(ui::ElementDocument* doc);
  void NotifySidebarChanged();
  void DetachCallChromeGesture();
  void AttachCallChromeGesture();
  void AttachCallStorySheetGesture();
  void ApplyCallStorySheetDetent();
  void ApplyLayoutModeFromContext(ui::Context* context);
  void OnLayoutModeChanged();
  int AllocatePaneId();
  int AllocateOverlayId();
  const PaneState* FindPane(const std::string& key) const;
  void SaveFocus();
  void RestoreFocus();
  void FlushPendingSyncLayout();
  void RemountCallChromeNow();
  void FlushRemountCallChrome();
  /** #166: the call layer's and the minimised chip's heights as laid out, for the chip's markup (ShellState). */
  void MeasureCallChip(ui::ElementDocument* doc);
  /** #166 diagnostics (call.chip): one line per call chrome remount while the chip is up (and the one leaving it). */
  void DiagCallChipRemount(ui::ElementDocument* doc, bool held);
  void RemountDialogChromeNow();
  void FlushRemountDialogChrome();
  void RemountPinGateChromeNow();
  /** SyncLayout rebuilt the DOM: put every MountOverlayBody body back into its (now empty) mount. */
  void RemountOverlayBodiesNow();
  void FlushRemountPinGateChrome();
  /** The AI drawer's host (#shell-ai-drawer-host): deferred (safe from a click), or now (SyncLayout). */
  void RemountAiDrawer();
  void FlushRemountAiDrawer();
  void RemountAiDrawerNow();
  /** The `ai_drawer` view into #shell-ai-drawer-mount, then its rows' swipes (one attachment per list id). */
  void MountAiDrawer();
  /** The AI page's rightward drag (ShellDrawerOpenGesture) on its minimal block, when the AI page shows. */
  void AttachDrawerOpenGesture();
  /** Composer blurred, keyboard and emoji panel down, the keyboard's inset dropped (OpenAiDrawer). */
  void ReleaseKeyboard();
  DismissTarget ResolveDismissTarget() const;
  void BeginAnimatedDismiss(DismissTarget target);
  /** Compact only: the element that slides out (and in) for `target`; null when it has no page to slide. */
  ui::Element* PageElementFor(DismissTarget target) const;
  /** Phone and the top overlay is a page sheet (PaneSpec::page_sheet). */
  bool TopPageSheet() const;
  /** A released drag moved a page sheet (PaneSpec::page_sheet) to `detent`. */
  void SetSheetDetent(int layer_id, SheetDetent detent);
  /** Slide the element with this id in from the right (adds the shell-page-enter class). */
  void PlayPageEnter(const char* element_id);
  void CommitDismiss(DismissTarget target);
  void ApplySafeAreaLayout();
  bool HomeDockApplies() const;
  const char* HomeDockWhy() const;
  void DiagHomeDock(float applied_lift_dp);
  void DiagHomeCache(const char* what, const char* why, float landing_h, float card_bottom);
  void SyncHomeDock(ui::ElementDocument* doc, bool dock);
  bool ChromeFrostEnabled() const;
  /** What is on screen now, for the AI dot (AiDotModel::Shown). */
  AiDotScene AiDotSceneNow() const;
  /** Tells SecureScreen when a secret comes on screen or goes (the PIN gate, the secret page). */
  void SyncSecureScreen();
  /** The AI dot's bound keys (ai_dot_*). */
  void DirtyAiDot();
  /** state_.ai_dot_look from the pref and today; true when it changed. */
  bool ResolveAiDotLook();
  struct SafeAreaFromSdl {
    int top_dp = 0;
    int bottom_dp = 0;
  };
  SafeAreaFromSdl ReadSafeAreaFromSdl() const;

  struct PendingDismiss {
    DismissTarget target = DismissTarget::None;
    std::chrono::steady_clock::time_point at;
  };

  ui::Context* context_ = nullptr;
  ShellState state_;
  ShellConfig config_;
  int safe_area_top_from_prefs_dp_ = 0;
  int safe_area_bottom_from_prefs_dp_ = 0;
  /** The AI Home measured with nothing up (SyncHomeDock): window height, landing height, card bottom (dp); 0 = unseen. */
  float home_cache_window_dp_ = 0.f;
  float home_cache_landing_h_dp_ = 0.f;
  float home_cache_card_bottom_dp_ = 0.f;
  std::string home_dock_last_diag_;
  std::string home_dock_last_cache_diag_;
  /** Last IME-sized bottom inset from SDL (latched while keyboard visible). */
  int last_ime_bottom_dp_ = 0;
  /** Synthetic bottom inset while bottom chrome (IME replacement) is open. */
  int bottom_chrome_height_dp_ = 0;
  /** The panel keeps a share of the window's height (BottomChromeSpec::height_share), not the keyboard's. */
  bool bottom_chrome_fixed_height_ = false;
  bool bottom_chrome_open_ = false;
  std::string bottom_chrome_key_;
  std::string bottom_chrome_rml_path_;
  bool remount_bottom_chrome_pending_ = false;
  std::function<void()> on_bottom_chrome_dismissed_;
  LayoutMode last_synced_mode_ = LayoutMode::Expanded;
  int next_pane_id_ = 1;
  int next_overlay_id_ = 1;
  float elapsed_ms_ = 0.f;
  std::optional<PendingDismiss> pending_dismiss_;
  /** Id of a page to slide in once the next SyncLayout has built it (transient page, chat overlay). */
  std::string page_enter_pending_id_;
  /** A page sheet just pushed: it gets .shell-page-sheet-enter (slide up) once the layout is built. */
  std::string sheet_enter_pending_id_;
  /** A swipe-back already started the slide-out; BeginAnimatedDismiss must not restart it. */
  bool slide_out_started_ = false;
  std::vector<LocalBackEntry> local_back_stack_;
  /** Phone: the tab-bar tab left for a hidden tab (Me, Contacts); HiddenTabBack returns to it. */
  NavTab tab_before_hidden_ = NavTab::Sessions;
  ui::String saved_focus_id_;
  bool sync_pending_ = false;
  /** Who asked for the pending / running SyncLayout (first request) and how many requests it merges. */
  std::string pending_sync_reason_;
  int pending_sync_requests_ = 0;
  std::string running_sync_reason_ = "direct";
  int running_sync_requests_ = 1;
  bool remount_call_chrome_pending_ = false;
  /** #166: a call chrome remount kept the minimised chip as it was (a drag still owned it): rebuild once it settles. */
  bool call_chip_rebuild_held_ = false;
  /** The chip's mode and place at the last call.chip remount line (its `reason`). */
  bool diag_chip_minimized_ = false;
  bool diag_chip_tucked_ = false;
  int diag_chip_side_ = 0;
  int diag_chip_y_ = 0;
  bool remount_dialog_chrome_pending_ = false;
  bool remount_pin_gate_chrome_pending_ = false;
  bool remount_ai_drawer_pending_ = false;
  /** The drawer was just opened: it slides in once mounted (not on a remount while it stays open). */
  bool drawer_enter_pending_ = false;
  /** Last body per overlay mount id (onboarding wizard); survives the full rebuild in SyncLayout. */
  std::map<std::string, std::string> overlay_bodies_;
  /** True from first paint until SettleStartupCoverArm (deferred unlock decision). */
  bool startup_cover_armed_ = true;
  /** Cleared after cold-start prepare ends — blocks mid-session unlock from re-showing the logo. */
  bool startup_cover_enabled_ = true;
  bool restore_focus_after_sync_ = false;
  ShellGestureAxisLock gesture_axis_lock_;
  ShellSwipeBackGesture swipe_back_gesture_;
  ShellPageSheetGesture page_sheet_gesture_;
  /** The phone call page's "how this call got through" sheet (#call-story-sheet) and the stop it stands at. */
  ShellPageSheetGesture call_story_sheet_gesture_;
  SheetDetent call_story_sheet_detent_ = SheetDetent::Large;
  ShellCallChromeGesture call_chrome_gesture_;
  ShellSplitterDrag splitter_drag_;
  ShellSplitterDrag aux_splitter_drag_;
  ShellLargeTitleScroll large_title_scroll_;
  /** Phone chat header and pushed pages: their bars collapse while the content scrolls. */
  ShellNavBarScroll nav_bar_scroll_;
  /** Home's 关注联系人 row: eases onto a whole person once its scroll is at rest. */
  ShellFavoritesSnap favorites_snap_;
  SessionSwipeGesture session_swipe_;
  /** The AI drawer's list (#ai-drawer-sessions): attached while the drawer is mounted, released when it closes. */
  SessionSwipeGesture drawer_session_swipe_;
  /** The New chat page's contact list (#new-chat-list): attached while that page is the top one, released with it. */
  SessionSwipeGesture new_chat_swipe_{ContactRowSwipeConfig()};
  ShellDrawerOpenGesture drawer_open_gesture_;
  std::function<void()> on_compact_chat_shown_again_;
  SessionSwipeGesture::ActionCallback on_session_swipe_action_;
  SessionSwipeGesture::NamedActionCallback on_new_chat_swipe_action_;
  std::function<void(const std::string&)> on_before_transient_mount_;
  std::function<void(const std::string&)> on_transient_mounted_;
  std::function<void(const std::string&)> on_transient_popped_;
  std::function<void(NavTab)> on_nav_tab_changed_;
  std::function<void(LayoutMode)> on_layout_mode_changed_;
  std::function<void(int, bool)> on_sidebar_changed_;
  std::function<void(int)> on_auxiliary_width_changed_;
  std::function<void()> on_layout_synced_;
  std::function<void(int64_t ms)> on_sync_layout_measured_;
  MessagingShellPorts shell_messaging_ports_;
  ShellStatusbar statusbar_{state_, context_, shell_messaging_ports_, [this]() { ApplySafeAreaLayout(); }};
  PinGateActionPorts pin_gate_actions_;
  FlowCoordinatorPorts flow_coordinator_;
  CallActionsPorts call_actions_;
  std::function<void()> scan_action_;
  ShellAiDot ai_dot_{state_};
  ShellHomeMascot home_mascot_;
  /** The look pref (`auto`, `a`-`d`) and when `auto` was last resolved to state_.ai_dot_look (a day can turn over). */
  std::string ai_dot_look_pref_ = "auto";
  int64_t ai_dot_look_checked_ms_ = 0;
  std::function<void()> on_ai_dot_tap_;
  std::function<void(bool, int, bool)> on_ai_dot_moved_;
  std::function<bool()> secret_page_;
  std::function<bool()> secret_legible_;
  /** What SecureScreen was last told. */
  bool secret_on_screen_ = false;
  bool secret_legible_on_ = false;
};

} // namespace pbr
