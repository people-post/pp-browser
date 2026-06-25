#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Types.h>

#include <functional>

namespace Rml {
class Context;
class Event;
}

namespace pbr {

class ShellChatOverlayGesture : public Rml::EventListener {
public:
  using DismissCallback = std::function<void()>;

  void Attach(Rml::Element* overlay, Rml::Context* context, float shell_width_dp, DismissCallback on_dismiss);
  void Detach();

  void ProcessEvent(Rml::Event& event) override;

private:
  bool ShouldIgnoreTarget(Rml::Element* target) const;
  void BeginDrag(int x);
  void UpdateDrag(int x, Rml::Event& event);
  void EndDrag();
  void SetOverlayOffset(float dx, bool animate);

  Rml::Element* overlay_ = nullptr;
  Rml::Context* context_ = nullptr;
  float shell_width_dp_ = 0.f;
  DismissCallback on_dismiss_;
  bool attached_ = false;
  bool tracking_ = false;
  bool dragging_ = false;
  int drag_start_x_ = 0;
  int drag_last_x_ = 0;
};

} // namespace pbr
