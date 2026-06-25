#pragma once

#include "base/ui/ShellTypes.h"

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
  void ToggleSecondary();
  void ToggleAuxiliary();
  void PushTransient(const PaneSpec& spec);
  void PopTransient();
  int PushLayer(const PaneSpec& spec);
  void CloseLayer(int layer_id = -1);
  bool HandleDismiss();

  void DirtyWindow();
  void SetActivityVisible(bool visible);
  void SetOnBeforeTransientMount(std::function<void(const std::string& key)> callback);
  void SetOnTransientMounted(std::function<void(const std::string& key)> callback);

  static void ToggleSecondaryCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ToggleAuxiliaryCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OpenAuxiliaryCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void PopTransientCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CloseLayerCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void DismissBannerCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void DialogOkCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void DialogCancelCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  static bool RegisterWindowModel(Rml::Context* context);

private:
  ShellHost() = default;

  Rml::Element* ShellRoot() const;
  std::string SerializeShellRoot() const;
  std::string SerializePaneSlot(const std::string& key, const char* extra_class, bool with_composer_slot = false) const;
  std::string SerializeExpandedBase() const;
  std::string SerializeCompactBase() const;
  std::string SerializeOverlays() const;
  std::string SerializeDialog() const;
  std::string SerializeTransientLayer() const;
  void MountPaneBodies();
  void MountComposer();
  void ApplyLayoutModeFromContext(Rml::Context* context);
  void OnLayoutModeChanged();
  int AllocatePaneId();
  int AllocateOverlayId();
  const PaneState* FindPane(const std::string& key) const;
  void SaveFocus();
  void RestoreFocus();
  void FlushPendingSyncLayout();

  Rml::Context* context_ = nullptr;
  ShellState state_;
  ShellConfig config_;
  LayoutMode last_synced_mode_ = LayoutMode::Expanded;
  int next_pane_id_ = 1;
  int next_overlay_id_ = 1;
  float elapsed_ms_ = 0.f;
  Rml::String saved_focus_id_;
  bool sync_pending_ = false;
  bool restore_focus_after_sync_ = false;
  std::function<void(const std::string&)> on_before_transient_mount_;
  std::function<void(const std::string&)> on_transient_mounted_;
};

} // namespace pbr
