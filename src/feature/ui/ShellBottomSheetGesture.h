#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Types.h>

#include <functional>

namespace Rml {
class Context;
class Event;
class ElementDocument;
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
  void BeginDrag(int y_px);
  void UpdateDrag(int y_px, Rml::Event& event);
  void EndDrag();
  void SetSheetOffset(float dy_dp, bool animate);
  void SetDocumentDragCapture(bool enabled);
  float PixelDeltaToDp(int delta_px) const;
  float ResolveSheetHeightDp() const;

  Rml::Element* sheet_ = nullptr;
  Rml::ElementDocument* document_ = nullptr;
  Rml::Context* context_ = nullptr;
  float sheet_height_dp_ = 0.f;
  DismissCallback on_dismiss_;
  bool attached_ = false;
  bool document_drag_capture_ = false;
  bool tracking_ = false;
  bool dragging_ = false;
  int drag_start_y_px_ = 0;
  int drag_last_y_px_ = 0;
};

} // namespace pbr
