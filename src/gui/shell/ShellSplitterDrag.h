#pragma once

#include <ui/dom/EventListener.h>

#include <functional>

namespace ui {
class Context;
class Element;
class ElementDocument;
class Event;
}

namespace pbr {

/**
 * Drag handle between the secondary and primary panes (expanded layout).
 *
 * Mousedown on the handle starts a drag; document-level capture listeners follow the pointer.
 * While dragging only the pane's inline `flex` is changed (no shell remount). `on_commit` fires once
 * on mouseup with the final clamped width in dp.
 */
class ShellSplitterDrag : public ui::EventListener {
public:
  using Commit = std::function<void(int width_dp)>;

  void Attach(ui::Element* handle, ui::Element* pane, ui::Context* context, int width_dp, Commit on_commit);
  void Detach();

  void ProcessEvent(ui::Event& event) override;

private:
  void SetDocumentCapture(bool enabled);
  void EndDrag(bool commit);

  ui::Element* handle_ = nullptr;
  ui::Element* pane_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ui::Context* context_ = nullptr;
  Commit on_commit_;
  bool document_capture_ = false;
  bool dragging_ = false;
  int start_x_px_ = 0;
  int start_width_dp_ = 0;
  int width_dp_ = 0;
};

} // namespace pbr
