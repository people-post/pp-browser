#pragma once

#include "base/data/SessionStore.h"
#include "base/ui/EmojiCatalog.h"
#include "common/Module.h"
#include "feature/ui/FlowCoordinatorPorts.h"
#include "feature/ui/ShellFeedbackPorts.h"
#include "feature/ui/ShellNavigationPorts.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

#include <functional>
#include <string>
#include <vector>

namespace Rml {
class Context;
class Element;
}

namespace pbr {

class EmojiPickerController : public Module {
public:
  enum class Mode { Insert, React };

  struct RailTab {
    Rml::String id;
    Rml::String glyph;
    bool active = false;
  };

  struct Cell {
    Rml::String glyph;
  };

  struct Section {
    Rml::String id;
    Rml::String element_id;
    Rml::String label;
    std::vector<Cell> cells;
  };

  EmojiPickerController();
  ~EmojiPickerController() override = default;

  static void InstallInstance(EmojiPickerController& controller);
  static void ClearInstance();
  static EmojiPickerController& Instance();

  void BindShellNavigation(ShellNavigationPorts ports);
  void BindShellFeedback(ShellFeedbackPorts ports);
  void BindFlowCoordinator(FlowCoordinatorPorts ports);
  void BindSessionStore(SessionStore& store);

  bool RegisterModel(Rml::Context* context);

  void OpenInsert(std::function<void(std::string emoji, bool restore_composer_focus)> on_pick);
  void OpenReact(std::string message_id, std::function<void(std::string emoji)> on_pick);
  void Close();

private:
  void OpenOverlayPresentation();
  void OpenBottomChromePresentation();
  void RegisterFlow();
  void OnFlowDismissed();
  void ResetState();
  void RebuildModel();
  void DirtyAll();
  void PersistRecent(const std::string& glyph);
  void OnEmojiPicked(const std::string& glyph);
  void UpdateActiveFromScroll();
  void ScrollToCategory(const std::string& category_id);
  void EnsureWindowAround(int center_index);
  bool IsBottomChromeEmojiOpen() const;
  Rml::Element* FindScrollBody() const;
  Rml::Element* FindSectionElement(const std::string& category_id) const;

  static void SelectEmojiCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args);
  static void SelectCategoryCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args);
  static void OnScrollCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);
  static void CancelCallback(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&);

  Rml::Context* context_ = nullptr;
  int layer_id_ = -1;
  Mode mode_ = Mode::Insert;
  /** True when Insert used mobile/compact bottom-chrome (IME slot) presentation. */
  bool bottom_chrome_mode_ = false;
  std::string react_message_id_;
  std::function<void(std::string, bool)> on_insert_pick_;
  std::function<void(std::string)> on_react_pick_;

  Rml::String title_;
  Rml::String active_category_;
  std::vector<RailTab> rail_tabs_;
  std::vector<Section> sections_;
  /** App-side window: bind cells for sections in [window_begin, window_end). */
  int window_begin_ = 0;
  int window_end_ = 0;

  ShellNavigationPorts shell_navigation_;
  ShellFeedbackPorts shell_feedback_;
  FlowCoordinatorPorts flow_coordinator_;
  SessionStore* session_store_ = nullptr;

  static EmojiPickerController* installed_instance_;
};

} // namespace pbr
