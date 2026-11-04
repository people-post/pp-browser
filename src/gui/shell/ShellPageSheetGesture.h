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
 *
 * A sheet with a half stop (PaneSpec::sheet_half_stop) also moves between half and full height: at half it follows
 * a drag up that starts outside scrolling content (the title bar, the grab handle), and a drag down from full goes
 * back to half (PageSheetModel::OnRelease); `on_detent` then records the new stop.
 */
struct ShellPageSheetOptions {
  bool half_stop = false;
  /** Where the sheet shows now (full while the keyboard is up); read at each press. */
  std::function<SheetDetent()> shown_detent;
  /** A released drag moved the sheet to the other stop. */
  std::function<void(SheetDetent)> on_detent;
};

class ShellPageSheetGesture : public ui::EventListener {
public:
  using Options = ShellPageSheetOptions;

  void Attach(ui::Element* sheet, ui::Context* context, std::function<void()> on_dismiss, Options options = {});
  void Detach();
  void ProcessEvent(ui::Event& event) override;

private:
  bool ShouldIgnoreTarget(ui::Element* target) const;
  bool ScrolledToTop(ui::Element* target) const;
  /** Pressed inside content that can scroll (an answer list with more to show): a drag up scrolls it. */
  bool InScrollingContent(ui::Element* target) const;
  void SetOffset(float dy_dp, bool animate);
  void SetDocumentCapture(bool enabled);
  void Release();
  float ToDp(int px) const;

  ui::Element* sheet_ = nullptr;
  ui::Element* press_target_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ui::Context* context_ = nullptr;
  std::function<void()> on_dismiss_;
  Options options_;
  /** The stop the sheet showed at the press, and its top edge then (dp from its containing block). */
  SheetDetent press_detent_ = SheetDetent::Full;
  float press_top_dp_ = 0.f;
  /** This press may drag the sheet up (a half-stop sheet at half, pressed outside scrolling content). */
  bool may_drag_up_ = false;
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
