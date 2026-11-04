#pragma once

#include "lib/ui/ShellTypes.h"
#include "foundation/data/UserPreferences.h"
#include "common/Module.h"
#include "gui/shell/ShellAiDot.h"
#include "gui/shell/ShellLayoutRml.h"
#include "gui/shell/ShellStatusbar.h"
#include "gui/shell/StatusbarText.h"
#include "gui/CallActionsPorts.h"
#include "gui/CallChromeSync.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/PinGateActionPorts.h"
#include "gui/shell/ShellCallChromePorts.h"
#include "gui/shell/ShellCallChromeGesture.h"
#include "lib/ui/ShellGestureAxis.h"
#include "gui/shell/ShellLargeTitleScroll.h"
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
}

namespace pbr {

enum class DismissStyle { Instant, Animated };

enum class DismissTarget {
  None,
  LocalBack,
  CompactChatOverlay,
  AuxiliarySheet,
  Transient,
  OverlayLayer,
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
    /** Phone AI dot: shown or not, and its look (`a`–`d`). */
    bool ai_dot = true;
    std::string ai_dot_look = "b";

    bool operator==(const ChromePrefs& other) const {
      return theme == other.theme && appearance == other.appearance &&
             reduce_transparency == other.reduce_transparency &&
             compact_chrome_frost == other.compact_chrome_frost && ai_dot == other.ai_dot &&
             ai_dot_look == other.ai_dot_look;
    }
    bool operator!=(const ChromePrefs& other) const { return !(*this == other); }
  };

  static ChromePrefs ProjectChrome(const ProfilePreferences& prefs);
  /** Apply material fields only (theme/appearance are Theme-owned). */
  void Apply(const ChromePrefs& prefs);

  ShellHost();


  void BindShellMessaging(MessagingShellPorts ports);
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
  void SelectNavTab(NavTab tab);
  /** Phone root pages: flips the large-title state; dirties the model only when it changes. */
  void SetLargeTitleCollapsed(bool collapsed);
  void ClearTabContext();
  void SetPrimaryPane(const std::string& key);
  void ClearPrimaryPane();
  void OpenCompactChat();
  void CloseCompactChat();
  void PushTransient(const PaneSpec& spec);
  void PopTransient();
  int PushLayer(const PaneSpec& spec);
  void CloseLayer(int layer_id = -1);

  /**
   * Single dismiss entry for Escape, back buttons, and swipe gestures.
   * Animated style schedules CommitDismiss after the exit transition.
   * When `force` is set, that target is dismissed (sheet swipe ignores local back).
   */
  bool RequestDismiss(DismissStyle style = DismissStyle::Instant,
                      DismissTarget force = DismissTarget::None);
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
  /** Phone Chats list: a swipe-row button (or a full swipe) was used on `thread_id`. */
  void SetOnSessionSwipeAction(SessionSwipeGesture::ActionCallback callback);

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

  /** Phone AI dot (DESIGN-pp-ui §8.14): the Me → AI assistant switch and look (`a`–`d`); takes effect at once. */
  void SetAiDot(bool enabled, const std::string& look);
  /** Where the AI dot rests, from disk (side, height in its band 0–1000). */
  void SetAiDotPlace(bool left, int y_permille);
  /** A tap on the AI dot (open the ask-AI panel). */
  void SetOnAiDotTap(std::function<void()> callback);
  /** The user dragged the AI dot to a new resting place (persist it). */
  void SetOnAiDotMoved(std::function<void(bool left, int y_permille)> callback);
  /** True while a page the AI dot must not sit on is up (Me → Account & security: change PIN, recovery phrase). */
  void SetAiDotSecretPage(std::function<bool()> showing);


  bool RegisterWindowModel(ui::Context* context);

private:
  // Data-model events with more to them than one call.
  void OnNavTabClicked(const std::string& tab_name);
  void DismissBanner();
  void DialogOk();
  void DialogCancel();
  void DialogToggleCheckbox();
  void PinGateSubmit();
  void PinGateCancel();
  void PinGateSetPin();
  void PinGateUseDefault();
  void PinGateIdentityNew();
  void PinGateIdentityLink();
  void PinGateIdentityPhrase();
  void PinGateUseSavedCopy();
  void PinGateForgotToggle();
  void CallAccept();
  void CallAcceptVoice();
  void CallAcceptCharge();
  void CallDecline();
  void CallLeave();
  void CallRetry();
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
  void RemountDialogChromeNow();
  void FlushRemountDialogChrome();
  void RemountPinGateChromeNow();
  /** SyncLayout rebuilt the DOM: put every MountOverlayBody body back into its (now empty) mount. */
  void RemountOverlayBodiesNow();
  void FlushRemountPinGateChrome();
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
  bool ChromeFrostEnabled() const;
  /** What is on screen now, for the AI dot (AiDotModel::Shown). */
  AiDotScene AiDotSceneNow() const;
  /** The AI dot's bound keys (ai_dot_*). */
  void DirtyAiDot();
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
  /** Last IME-sized bottom inset from SDL (latched while keyboard visible). */
  int last_ime_bottom_dp_ = 0;
  /** Synthetic bottom inset while bottom chrome (IME replacement) is open. */
  int bottom_chrome_height_dp_ = 0;
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
  ui::String saved_focus_id_;
  bool sync_pending_ = false;
  /** Who asked for the pending / running SyncLayout (first request) and how many requests it merges. */
  std::string pending_sync_reason_;
  int pending_sync_requests_ = 0;
  std::string running_sync_reason_ = "direct";
  int running_sync_requests_ = 1;
  bool remount_call_chrome_pending_ = false;
  bool remount_dialog_chrome_pending_ = false;
  bool remount_pin_gate_chrome_pending_ = false;
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
  ShellCallChromeGesture call_chrome_gesture_;
  ShellSplitterDrag splitter_drag_;
  ShellSplitterDrag aux_splitter_drag_;
  ShellLargeTitleScroll large_title_scroll_;
  SessionSwipeGesture session_swipe_;
  SessionSwipeGesture::ActionCallback on_session_swipe_action_;
  std::function<void(const std::string&)> on_before_transient_mount_;
  std::function<void(const std::string&)> on_transient_mounted_;
  std::function<void(const std::string&)> on_transient_popped_;
  std::function<void(NavTab)> on_nav_tab_changed_;
  std::function<void(LayoutMode)> on_layout_mode_changed_;
  std::function<void(int, bool)> on_sidebar_changed_;
  std::function<void(int)> on_auxiliary_width_changed_;
  std::function<void()> on_layout_synced_;
  MessagingShellPorts shell_messaging_ports_;
  ShellStatusbar statusbar_{state_, context_, shell_messaging_ports_, [this]() { ApplySafeAreaLayout(); }};
  PinGateActionPorts pin_gate_actions_;
  FlowCoordinatorPorts flow_coordinator_;
  CallActionsPorts call_actions_;
  std::function<void()> scan_action_;
  ShellAiDot ai_dot_{state_};
  std::function<void()> on_ai_dot_tap_;
  std::function<void(bool, int)> on_ai_dot_moved_;
  std::function<bool()> ai_dot_secret_page_;

};

} // namespace pbr
