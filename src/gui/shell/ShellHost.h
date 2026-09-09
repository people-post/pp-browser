#pragma once

#include "domain/ui/ShellTypes.h"
#include "foundation/data/UserPreferences.h"
#include "common/Module.h"
#include "feature/conversations/MessagingShellPorts.h"
#include "gui/CallActionsPorts.h"
#include "gui/CallChromeSync.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/PinGateActionPorts.h"
#include "gui/shell/ShellCallChromePorts.h"
#include "gui/shell/ShellBottomSheetGesture.h"
#include "gui/shell/ShellCallChromeGesture.h"
#include "gui/shell/ShellGestureAxis.h"
#include "gui/shell/ShellSwipeBackGesture.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace Rml {
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
  AccountSheet,
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

    bool operator==(const ChromePrefs& other) const {
      return theme == other.theme && appearance == other.appearance &&
             reduce_transparency == other.reduce_transparency &&
             compact_chrome_frost == other.compact_chrome_frost;
    }
    bool operator!=(const ChromePrefs& other) const { return !(*this == other); }
  };

  static ChromePrefs ProjectChrome(const ProfilePreferences& prefs);
  /** Apply material fields only (theme/appearance are Theme-owned). */
  void Apply(const ChromePrefs& prefs);

  ShellHost();

  /** App-owned instance; set via InstallInstance from Application. Static callbacks use Instance(). */
  static void InstallInstance(ShellHost& host);
  static void ClearInstance();
  static ShellHost& Instance();

  void BindShellMessaging(MessagingShellPorts ports);
  void OpenStatusbarPopover();
  void CloseStatusbarPopover();
  void ToggleStatusbarPopover();
  void BindPinGateActions(PinGateActionPorts ports);
  void BindFlowCoordinator(FlowCoordinatorPorts ports);
  void BindCallActions(CallActionsPorts ports);

  void Initialize(Rml::Context* context);
  void SyncLayout();
  void RequestSyncLayout(bool restore_focus_after = false, const char* reason = nullptr);
  /** Mount/clear call ring + in-call overlays without remounting the full shell tree.
   *  Defers to the next UI turn so Rml click handlers are not mid-dispatch on destroyed nodes. */
  void RemountCallChrome();
  /** Mount/clear alert/confirm/prompt into #shell-dialog-mount (not full SyncLayout). */
  void RemountDialogChrome();
  /** Mount/clear PIN gate into #shell-pin-gate-mount (not full SyncLayout). */
  void RemountPinGateChrome();
  void Update(Rml::Context* context);
  /** Call after Rml::Context::Update so RequestNextUpdate is not cleared by it. Arms power-save. */
  void NotifyFrameEnd(Rml::Context* context);

  ShellState& State() { return state_; }
  const ShellState& State() const { return state_; }

  void RegisterPane(const PaneSpec& spec);
  void SetAuxiliaryAvailable(bool available);
  void OpenAuxiliary();
  void CloseAuxiliary();
  void ToggleAuxiliary();
  void SelectNavTab(NavTab tab);
  void ClearTabContext();
  void SetPrimaryPane(const std::string& key);
  void ClearPrimaryPane();
  void OpenCompactChat();
  void CloseCompactChat();
  void OpenAccountSheet();
  void CloseAccountSheet();
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
  void DirtyStatusChrome();
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
  void SetActivity(bool visible, const Rml::String& message = {});
  void SetOnBeforeTransientMount(std::function<void(const std::string& key)> callback);
  void SetOnTransientMounted(std::function<void(const std::string& key)> callback);
  void SetOnTransientPopped(std::function<void(const std::string& key)> callback);
  void SetOnNavTabChanged(std::function<void(NavTab tab)> callback);
  void SetOnLayoutModeChanged(std::function<void(LayoutMode mode)> callback);
  void SetOnLayoutSynced(std::function<void()> callback);
  void SetOnAccountSheetOpened(std::function<void()> callback);
  void SetOnAccountSheetClosed(std::function<void()> callback);

  /** Seed safe-area insets from machine.json (used when SDL reports zero). */
  void SetSafeAreaInsetsFromPrefs(int top_dp, int bottom_dp);
  void RefreshSafeAreaInsets(Rml::Context* context);

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

  static void ToggleAuxiliaryCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OpenAuxiliaryCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SelectNavTabCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CompactChatBackCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OpenAccountSheetCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CloseAccountSheetCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void PopTransientCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CloseLayerCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void DismissBannerCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void DialogOkCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void DialogCancelCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void DialogToggleCheckboxCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void PinGateSubmitCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void PinGateCancelCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void PinGateSetPinCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void PinGateUseDefaultCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void PinGateIdentityNewCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void PinGateIdentityLinkCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CallAcceptCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CallAcceptChargeCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CallDeclineCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CallLeaveCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CallRetryCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CallMuteCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CallCameraCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CallSpeakerCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CallInviteCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CallMinimizeCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CallExpandCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CallImmersiveCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CallRestoreCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CallDetailsCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void TitlebarMinimizeCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void TitlebarToggleMaximizeCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void TitlebarCloseCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ToggleStatusbarPopoverCallback(Rml::DataModelHandle model, Rml::Event& ev,
                                             const Rml::VariantList& args);
  static void DismissStatusbarPopoverCallback(Rml::DataModelHandle model, Rml::Event& ev,
                                              const Rml::VariantList& args);
  static void RetestStatusbarReachabilityCallback(Rml::DataModelHandle model, Rml::Event& ev,
                                                  const Rml::VariantList& args);
  static void OpenNetworkSettingsCallback(Rml::DataModelHandle model, Rml::Event& ev,
                                          const Rml::VariantList& args);

  bool RegisterWindowModel(Rml::Context* context);

private:

  Rml::Element* ShellRoot() const;
  std::string SerializeShellRoot() const;
  std::string SerializePaneSlot(const std::string& key, const char* extra_class, bool with_composer_slot = false) const;
  std::string SerializeExpandedBase() const;
  std::string SerializeCompactBase() const;
  std::string SerializeAccountSheet() const;
  std::string SerializeOverlays() const;
  std::string SerializeBottomChrome() const;
  std::string SerializeDialog() const;
  std::string SerializePinGate() const;
  std::string SerializeCallRing() const;
  std::string SerializeCallInProgress() const;
  std::string SerializeTransientLayer() const;
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
  void AttachAccountSheetGesture();
  void DetachCallChromeGesture();
  void AttachCallChromeGesture();
  void ApplyLayoutModeFromContext(Rml::Context* context);
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
  void FlushRemountPinGateChrome();
  DismissTarget ResolveDismissTarget() const;
  void BeginAnimatedDismiss(DismissTarget target);
  void CommitDismiss(DismissTarget target);
  void ApplySafeAreaLayout();
  void RefreshStatusbarVisibility();
  void RefreshStatusbarCluster();
  void ClearStatusbarCluster();
  bool ApplyStatusbarCluster(const StatusbarClusterSnapshot& snap);
  void ClearStatusbarPopover();
  bool ApplyStatusbarPopover(const StatusbarPopoverSnapshot& snap);
  void RefreshStatusbarPopover();
  void PositionStatusbarPopover();
  void DirtyStatusbarPopover();
  bool ChromeFrostEnabled() const;
  struct SafeAreaFromSdl {
    int top_dp = 0;
    int bottom_dp = 0;
  };
  SafeAreaFromSdl ReadSafeAreaFromSdl() const;

  struct PendingDismiss {
    DismissTarget target = DismissTarget::None;
    std::chrono::steady_clock::time_point at;
  };

  Rml::Context* context_ = nullptr;
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
  std::vector<LocalBackEntry> local_back_stack_;
  Rml::String saved_focus_id_;
  bool sync_pending_ = false;
  bool remount_call_chrome_pending_ = false;
  bool remount_dialog_chrome_pending_ = false;
  bool remount_pin_gate_chrome_pending_ = false;
  /** True from first paint until SettleStartupCoverArm (deferred unlock decision). */
  bool startup_cover_armed_ = true;
  /** Cleared after cold-start prepare ends — blocks mid-session unlock from re-showing the logo. */
  bool startup_cover_enabled_ = true;
  bool restore_focus_after_sync_ = false;
  ShellGestureAxisLock gesture_axis_lock_;
  ShellSwipeBackGesture swipe_back_gesture_;
  ShellBottomSheetGesture account_sheet_gesture_;
  ShellCallChromeGesture call_chrome_gesture_;
  std::function<void(const std::string&)> on_before_transient_mount_;
  std::function<void(const std::string&)> on_transient_mounted_;
  std::function<void(const std::string&)> on_transient_popped_;
  std::function<void(NavTab)> on_nav_tab_changed_;
  std::function<void(LayoutMode)> on_layout_mode_changed_;
  std::function<void()> on_layout_synced_;
  std::function<void()> on_account_sheet_opened_;
  std::function<void()> on_account_sheet_closed_;
  MessagingShellPorts shell_messaging_ports_;
  bool statusbar_popover_needs_position_ = false;
  PinGateActionPorts pin_gate_actions_;
  FlowCoordinatorPorts flow_coordinator_;
  CallActionsPorts call_actions_;

  static ShellHost* installed_instance_;
};

} // namespace pbr
