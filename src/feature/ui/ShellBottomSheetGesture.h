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

class ShellBottomSheetGesture : public Rml::EventListener {
public:
  using DismissCallback = std::function<void()>;

  void Attach(Rml::Element* sheet, Rml::Context* context, float sheet_height_dp, DismissCallback on_dismiss);
  void Detach();

  void ProcessEvent(Rml::Event& event) override;

private:
  bool ShouldIgnoreTarget(Rml::Element* target) const;
  bool ShouldStartSwipe(Rml::Element* target) const;
  void BeginDrag(int y);
  void UpdateDrag(int y, Rml::Event& event);
  void EndDrag();
  void SetSheetOffset(float dy, bool animate);

  Rml::Element* sheet_ = nullptr;
  Rml::Context* context_ = nullptr;
  float sheet_height_dp_ = 0.f;
  DismissCallback on_dismiss_;
  bool attached_ = false;
  bool tracking_ = false;
  bool dragging_ = false;
  int drag_start_y_ = 0;
  int drag_last_y_ = 0;
};

} // namespace pbr
