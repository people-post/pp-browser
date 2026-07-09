#pragma once

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Vector2.h>

#include <functional>
#include <string>
#include <vector>

namespace Rml {
class Context;
class Element;
}

namespace pbr {

struct ContextMenuAction {
  std::string id;
  std::string label;
  std::function<bool()> enabled;
  std::function<void()> run;
};

struct ContextMenuRequest {
  Rml::Vector2i position;
  Rml::Element* target = nullptr;
  Rml::Context* context = nullptr;
};

class ContextMenuHost : public Rml::EventListener {
public:
  static ContextMenuHost& Instance();

  void Install(Rml::Context* context);
  void RegisterProvider(std::function<std::vector<ContextMenuAction>(const ContextMenuRequest&)> provider);
  void ShowAt(const ContextMenuRequest& request);
  /// Show an explicit action list (no copy/select/paste text actions).
  void ShowActions(Rml::Vector2i position, std::vector<ContextMenuAction> actions);
  void Dismiss();
  bool IsOpen() const { return layer_ != nullptr; }
  bool HandleDismiss();

  void OnLongPress(Rml::Vector2i position, Rml::Element* target);
  bool OnContextPointer(Rml::Context* context, int x, int y);

private:
  void ProcessEvent(Rml::Event& event) override;
  std::vector<ContextMenuAction> CollectActions(const ContextMenuRequest& request) const;
  std::vector<ContextMenuAction> BuildTextActions() const;
  void RenderMenu(const ContextMenuRequest& request, const std::vector<ContextMenuAction>& actions);
  int FindMenuItemIndex(Rml::Element* target) const;
  void HandleMenuAction(int index);

  Rml::Context* context_ = nullptr;
  Rml::Context* menu_context_ = nullptr;
  Rml::Element* menu_target_ = nullptr;
  Rml::Element* menu_editor_ = nullptr;
  Rml::Element* layer_ = nullptr;
  Rml::Element* panel_ = nullptr;
  std::string copy_snapshot_;
  std::vector<ContextMenuAction> active_actions_;
  std::vector<std::function<std::vector<ContextMenuAction>(const ContextMenuRequest&)>> providers_;
};

} // namespace pbr
