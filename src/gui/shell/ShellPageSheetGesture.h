#pragma once

#include "gui/shell/PageSheetModel.h"

#include <ui/dom/EventListener.h>

#include <chrono>
#include <functional>

namespace ui {
class Context;
class Element;
class ElementDocument;
class Event;
}

namespace pbr {

/**
 * Swipe down to close a phone page sheet (ShellLayoutRml, PaneSpec::page_sheet). The sheet follows the finger
 * from anywhere on it (not from fields or buttons) while its content is scrolled to the top; released past
 * PageSheetModel's threshold or flicked it slides out and `on_dismiss` runs, otherwise it springs back.
 */
class ShellPageSheetGesture : public ui::EventListener {
public:
  void Attach(ui::Element* sheet, ui::Context* context, std::function<void()> on_dismiss);
  void Detach();
  void ProcessEvent(ui::Event& event) override;

private:
  bool ShouldIgnoreTarget(ui::Element* target) const;
  bool ScrolledToTop(ui::Element* target) const;
  void SetOffset(float dy_dp, bool animate);
  void SetDocumentCapture(bool enabled);
  void Release();
  float ToDp(int px) const;

  ui::Element* sheet_ = nullptr;
  ui::Element* press_target_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ui::Context* context_ = nullptr;
  std::function<void()> on_dismiss_;
  PageSheetModel::Velocity velocity_;
  bool document_capture_ = false;
  bool tracking_ = false;
  bool dragging_ = false;
  bool swallow_click_ = false;
  std::chrono::steady_clock::time_point swallow_since_{};
  int start_x_px_ = 0;
  int start_y_px_ = 0;
  int last_y_px_ = 0;
};

} // namespace pbr
