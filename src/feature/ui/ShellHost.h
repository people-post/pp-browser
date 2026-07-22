#pragma once

#include "base/ui/ShellTypes.h"
#include "feature/ui/ShellBottomSheetGesture.h"
#include "feature/ui/ShellChatOverlayGesture.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

#include <functional>
#include <string>

namespace Rml {
class Context;
class Element;
}

namespace pbr {

class ShellHost {
public:
  static ShellHost& Instance();

  void Initialize(Rml::Context* context);
  void SyncLayout();
  void RequestSyncLayout(bool restore_focus_after = false);
  void Update(Rml::Context* context);

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
  bool HandleDismiss();

  void DirtyWindow();
  // Deferred remount of the nav rail (safe from click handlers). Use when badge counts change
  // and DirtyVariable alone may not refresh data-if views.
  void RequestRemountNavRail();
  void SetActivityVisible(bool visible);
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
  std::string SerializeTransientLayer() const;
  const char* NavContentKey() const;
  void MountPaneBodies();
  void MountNavRail();
  void MountNavContent();
  void MountComposer();
  void AttachChatOverlayGesture();
  void DetachChatOverlayGesture();
  void AttachAccountSheetGesture();
  void DetachAccountSheetGesture();
  void ApplyLayoutModeFromContext(Rml::Context* context);
  void OnLayoutModeChanged();
  int AllocatePaneId();
  int AllocateOverlayId();
  const PaneState* FindPane(const std::string& key) const;
  void SaveFocus();
  void RestoreFocus();
  void FlushPendingSyncLayout();
  void ScheduleCompactChatDismiss();
  void ScheduleAccountSheetDismiss();
  void ApplySafeAreaLayout();
  bool ChromeFrostEnabled() const;
  struct SafeAreaFromSdl {
    int top_dp = 0;
    int bottom_dp = 0;
  };
  SafeAreaFromSdl ReadSafeAreaFromSdl() const;

  Rml::Context* context_ = nullptr;
  ShellState state_;
  ShellConfig config_;
  int safe_area_top_from_prefs_dp_ = 0;
  int safe_area_bottom_from_prefs_dp_ = 0;
  LayoutMode last_synced_mode_ = LayoutMode::Expanded;
  int next_pane_id_ = 1;
  int next_overlay_id_ = 1;
  float elapsed_ms_ = 0.f;
  float compact_chat_dismiss_at_ms_ = -1.f;
  float account_sheet_dismiss_at_ms_ = -1.f;
  Rml::String saved_focus_id_;
  bool sync_pending_ = false;
  bool restore_focus_after_sync_ = false;
  ShellChatOverlayGesture chat_overlay_gesture_;
  ShellBottomSheetGesture account_sheet_gesture_;
  std::function<void(const std::string&)> on_before_transient_mount_;
  std::function<void(const std::string&)> on_transient_mounted_;
  std::function<void(const std::string&)> on_transient_popped_;
  std::function<void(NavTab)> on_nav_tab_changed_;
  std::function<void(LayoutMode)> on_layout_mode_changed_;
  std::function<void()> on_layout_synced_;
};

} // namespace pbr
