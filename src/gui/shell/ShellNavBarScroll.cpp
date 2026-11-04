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
constexpr const char* kPageScrollClass = "nav-title-scroll";
constexpr const char* kPageHostClass = "nav-title-host";
constexpr const char* kPageCollapsedClass = "nav-title-collapsed";

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
  ui::ElementList scrollers;
  document_->GetElementsByClassName(scrollers, kPageScrollClass);
  for (ui::Element* scroller : scrollers) {
    OnPageScroll(scroller);
  }
  ui::Element* overlay = document_->GetElementById(kChatOverlayId);
  if (!overlay) {
    chat_.Reset();
    return;
  }
  // A remount (a sheet opening over the chat) rebuilds the transcript, whose scroll position comes back only later:
  // keep the state it had and let the next scroll decide. A chat that closed starts the next one expanded.
  overlay->SetClass(kChatCompactClass, chat_.compact);
}

void ShellNavBarScroll::ProcessEvent(ui::Event& event) {
  ui::Element* target = event.GetTargetElement();
  if (!target) {
    return;
  }
  if (target->GetId() == kTranscriptId) {
    OnChatScroll(target);
  } else if (target->IsClassSet(kPageScrollClass)) {
    OnPageScroll(target);
  }
}

void ShellNavBarScroll::OnPageScroll(ui::Element* scroller) {
  ui::Element* host = scroller->GetParentNode();
  while (host && !host->IsClassSet(kPageHostClass)) {
    host = host->GetParentNode();
  }
  if (!host) {
    return;  // a page sheet's copy of the page: it keeps its own title row
  }
  const bool collapsed = NavBarCollapseModel::PageTitleCollapsed(scroller->GetScrollTop() / DpRatio(scroller));
  if (host->IsClassSet(kPageCollapsedClass) != collapsed) {
    host->SetClass(kPageCollapsedClass, collapsed);
  }
}

void ShellNavBarScroll::OnChatScroll(ui::Element* transcript) {
  // Only the phone's chat page has the overlay; the desktop transcript has no collapsing header.
  ui::Element* overlay = document_ ? document_->GetElementById(kChatOverlayId) : nullptr;
  if (!overlay) {
    return;
  }
  const float max_top = std::max(0.f, transcript->GetScrollHeight() - transcript->GetClientHeight());
  // Clamped: the rubber band past the oldest message springing back is not a move towards the newest.
  const float from_newest_dp = std::clamp(max_top - transcript->GetScrollTop(), 0.f, max_top) / DpRatio(transcript);
  if (chat_.OnScroll(from_newest_dp)) {
    overlay->SetClass(kChatCompactClass, chat_.compact);
  }
}

} // namespace pbr
