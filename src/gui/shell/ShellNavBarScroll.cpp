#include "gui/shell/ShellNavBarScroll.h"

#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>

#include <algorithm>

namespace pbr {

namespace {

constexpr const char* kChatOverlayId = "shell-chat-overlay";
constexpr const char* kTranscriptId = "chat-messages";
constexpr const char* kChatCompactClass = "chat-bar-compact";

float DpRatio(ui::Element* element) {
  ui::Context* context = element ? element->GetContext() : nullptr;
  const float ratio = context ? context->GetDensityIndependentPixelRatio() : 1.f;
  return ratio > 0.f ? ratio : 1.f;
}

} // namespace

void ShellNavBarScroll::Attach(ui::ElementDocument* document) {
  Detach();
  if (!document) {
    return;
  }
  document_ = document->GetObserverPtr();
  document->AddEventListener(ui::EventId::Scroll, this);
  Refresh();
}

void ShellNavBarScroll::Detach() {
  if (document_) {
    document_->RemoveEventListener(ui::EventId::Scroll, this);
  }
  document_ = nullptr;
}

void ShellNavBarScroll::Refresh() {
  if (!document_) {
    return;
  }
  ui::Element* overlay = document_->GetElementById(kChatOverlayId);
  if (!overlay) {
    chat_.Reset();
    return;
  }
  if (ui::Element* transcript = document_->GetElementById(kTranscriptId)) {
    OnChatScroll(transcript);
  }
  overlay->SetClass(kChatCompactClass, chat_.compact);
}

void ShellNavBarScroll::ProcessEvent(ui::Event& event) {
  ui::Element* target = event.GetTargetElement();
  if (target && target->GetId() == kTranscriptId) {
    OnChatScroll(target);
  }
}

void ShellNavBarScroll::OnChatScroll(ui::Element* transcript) {
  // Only the phone's chat page has the overlay; the desktop transcript has no collapsing header.
  ui::Element* overlay = document_ ? document_->GetElementById(kChatOverlayId) : nullptr;
  if (!overlay) {
    return;
  }
  const float max_top = std::max(0.f, transcript->GetScrollHeight() - transcript->GetClientHeight());
  const float from_newest_dp = std::max(0.f, max_top - transcript->GetScrollTop()) / DpRatio(transcript);
  if (chat_.OnScroll(from_newest_dp)) {
    overlay->SetClass(kChatCompactClass, chat_.compact);
  }
}

} // namespace pbr
