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
} // namespace ui

namespace pbr {

/**
 * Drags a phone page sheet (ShellLayoutRml, PaneSpec::page_sheet) between its stops (PageSheetModel). A drag down
 * follows the finger from anywhere on the sheet (not from fields or buttons) while its content is scrolled to the
 * top; a drag up, below full height, only from outside scrolling content (the grab handle, the title row), so a list
 * still scrolls. Released, the sheet goes to the nearest stop (`on_detent` records a new one), or, let go low or
 * flicked down from small, it slides out and `on_dismiss` runs.
 */
struct ShellPageSheetOptions {
  /** Where the sheet shows now (full while the keyboard is up); read at each press. */
  std::function<SheetDetent()> shown_detent;
  /** A released drag moved the sheet to another stop. */
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
  /** This press may drag the sheet up (below full height, pressed outside scrolling content). */
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
