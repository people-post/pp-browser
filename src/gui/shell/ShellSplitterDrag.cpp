#include "gui/shell/ShellSplitterDrag.h"

#include "domain/ui/ShellLayout.h"

#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>

#include <cmath>
#include <cstdio>

namespace pbr {

namespace {

constexpr const char* kActiveClass = "shell-splitter--active";

bool IsButtonTarget(ui::Element* target, ui::Element* handle) {
  for (ui::Element* node = target; node && node != handle; node = node->GetParentNode()) {
    if (node->GetTagName() == "button") {
      return true;
    }
  }
  return false;
}

} // namespace

void ShellSplitterDrag::Attach(ui::Element* handle, ui::Element* pane, ui::Context* context, int width_dp,
                               Commit on_commit) {
  Detach();
  if (!handle || !pane || !context) {
    return;
  }
  handle_ = handle;
  pane_ = pane;
  context_ = context;
  document_ = handle->GetOwnerDocument();
  width_dp_ = width_dp;
  on_commit_ = std::move(on_commit);
  handle_->AddEventListener(ui::EventId::Mousedown, this);
}

void ShellSplitterDrag::Detach() {
  EndDrag(false);
  if (handle_) {
    handle_->RemoveEventListener(ui::EventId::Mousedown, this);
  }
  handle_ = nullptr;
  pane_ = nullptr;
  document_ = nullptr;
  context_ = nullptr;
  on_commit_ = nullptr;
}

void ShellSplitterDrag::SetDocumentCapture(bool enabled) {
  if (!document_ || document_capture_ == enabled) {
    return;
  }
  if (enabled) {
    document_->AddEventListener(ui::EventId::Mousemove, this, true);
    document_->AddEventListener(ui::EventId::Mouseup, this, true);
  } else {
    document_->RemoveEventListener(ui::EventId::Mousemove, this, true);
    document_->RemoveEventListener(ui::EventId::Mouseup, this, true);
  }
  document_capture_ = enabled;
}

void ShellSplitterDrag::EndDrag(bool commit) {
  SetDocumentCapture(false);
  if (!dragging_) {
    return;
  }
  dragging_ = false;
  if (handle_) {
    handle_->SetClass(kActiveClass, false);
  }
  if (commit && on_commit_) {
    on_commit_(width_dp_);
  }
}

void ShellSplitterDrag::ProcessEvent(ui::Event& event) {
  switch (event.GetId()) {
  case ui::EventId::Mousedown: {
    if (dragging_ || !handle_ || !pane_ || !context_ || IsButtonTarget(event.GetTargetElement(), handle_)) {
      return;
    }
    dragging_ = true;
    start_x_px_ = event.GetParameter<int>("mouse_x", 0);
    start_width_dp_ = width_dp_;
    handle_->SetClass(kActiveClass, true);
    SetDocumentCapture(true);
    break;
  }
  case ui::EventId::Mousemove: {
    if (!dragging_ || !pane_) {
      return;
    }
    const float ratio = context_->GetDensityIndependentPixelRatio();
    const int dx_px = event.GetParameter<int>("mouse_x", start_x_px_) - start_x_px_;
    const float dx_dp = ratio > 0.f ? static_cast<float>(dx_px) / ratio : static_cast<float>(dx_px);
    width_dp_ = ShellLayout::ClampSidebarWidthDp(start_width_dp_ + static_cast<int>(std::lround(dx_dp)));
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0 0 %ddp", width_dp_);
    pane_->SetProperty("flex", buffer);
    break;
  }
  case ui::EventId::Mouseup:
    EndDrag(true);
    break;
  default:
    break;
  }
}

} // namespace pbr
