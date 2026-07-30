#pragma once

#include "base/ui/ShellTypes.h"
#include "feature/ui/ShellBottomSheetGesture.h"
#include "feature/ui/ShellGestureAxis.h"
#include "feature/ui/ShellSwipeBackGesture.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Rml {
class Context;
class Element;
}

namespace pbr {

class MessagingHub;

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

class ShellHost {
public:
  static ShellHost& Instance();

  void BindMessaging(MessagingHub& messaging);
  MessagingHub& Hub();
  const MessagingHub& Hub() const;

  void Initialize(Rml::Context* context);
  void SyncLayout();
  void RequestSyncLayout(bool restore_focus_after = false, const char* reason = nullptr);
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

  void DirtyWindow();
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

  /** Seed safe-area insets from machine.json (used when SDL reports zero). */
  void SetSafeAreaInsetsFromPrefs(int top_dp, int bottom_dp);
  void RefreshSafeAreaInsets(Rml::Context* context);

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
  static void TitlebarMinimizeCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void TitlebarToggleMaximizeCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void TitlebarCloseCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  static bool RegisterWindowModel(Rml::Context* context);

private:
  ShellHost() = default;

  Rml::Element* ShellRoot() const;
  std::string SerializeShellRoot() const;
  std::string SerializePaneSlot(const std::string& key, const char* extra_class, bool with_composer_slot = false) const;
  std::string SerializeExpandedBase() const;
  std::string SerializeCompactBase() const;
  std::string SerializeAccountSheet() const;
  std::string SerializeOverlays() const;
  std::string SerializeDialog() const;
  std::string SerializePinGate() const;
  std::string SerializeCallRing() const;
  std::string SerializeCallInProgress() const;
  std::string SerializeTransientLayer() const;
  const char* NavContentKey() const;
  void MountPaneBodies();
  void MountNavRail();
  void MountNavContent();
  void MountComposer();
  void DetachDismissGestures();
  void AttachSwipeBackGesture();
  void AttachAccountSheetGesture();
  void ApplyLayoutModeFromContext(Rml::Context* context);
  void OnLayoutModeChanged();
  int AllocatePaneId();
  int AllocateOverlayId();
  const PaneState* FindPane(const std::string& key) const;
  void SaveFocus();
  void RestoreFocus();
  void FlushPendingSyncLayout();
  DismissTarget ResolveDismissTarget() const;
  void BeginAnimatedDismiss(DismissTarget target);
  void CommitDismiss(DismissTarget target);
  void ApplySafeAreaLayout();
  void RefreshStatusbarVisibility();
  void RefreshStatusbarConnection();
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
  LayoutMode last_synced_mode_ = LayoutMode::Expanded;
  int next_pane_id_ = 1;
  int next_overlay_id_ = 1;
  float elapsed_ms_ = 0.f;
  std::optional<PendingDismiss> pending_dismiss_;
  std::vector<LocalBackEntry> local_back_stack_;
  Rml::String saved_focus_id_;
  bool sync_pending_ = false;
  bool restore_focus_after_sync_ = false;
  ShellGestureAxisLock gesture_axis_lock_;
  ShellSwipeBackGesture swipe_back_gesture_;
  ShellBottomSheetGesture account_sheet_gesture_;
  std::function<void(const std::string&)> on_before_transient_mount_;
  std::function<void(const std::string&)> on_transient_mounted_;
  std::function<void(const std::string&)> on_transient_popped_;
  std::function<void(NavTab)> on_nav_tab_changed_;
  std::function<void(LayoutMode)> on_layout_mode_changed_;
  std::function<void()> on_layout_synced_;
  MessagingHub* messaging_ = nullptr;

};

} // namespace pbr
