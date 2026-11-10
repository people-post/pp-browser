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
constexpr const char* kChatHiddenClass = "chat-bar-hidden";
constexpr const char* kPageScrollClass = "nav-bar-scroll";
constexpr const char* kPageHostClass = "nav-bar-host";
constexpr const char* kPageScrolledClass = "nav-bar-scrolled";
constexpr const char* kPageHiddenClass = "nav-bar-hidden";

float DpRatio(ui::Element* element) {
  ui::Context* context = element ? element->GetContext() : nullptr;
  const float ratio = context ? context->GetDensityIndependentPixelRatio() : 1.f;
  return ratio > 0.f ? ratio : 1.f;
}

/** How far `scroller` is from its top or (`from_bottom`) its bottom, in dp; a rubber band past either end counts as
 *  the end (its spring back is no move). */
float FromHomeDp(ui::Element* scroller, const bool from_bottom) {
  const float max_top = std::max(0.f, scroller->GetScrollHeight() - scroller->GetClientHeight());
  const float top = std::clamp(scroller->GetScrollTop(), 0.f, max_top);
  return (from_bottom ? max_top - top : top) / DpRatio(scroller);
}

void SetClassIfChanged(ui::Element* element, const char* name, const bool on) {
  if (element->IsClassSet(name) != on) {
    element->SetClass(name, on);
  }
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
  overlay_ = nullptr;
}

void ShellNavBarScroll::Refresh() {
  if (!document_) {
    return;
  }
  // The pages were built again: each shows its bar; clear or not by where its content is.
  page_host_ = nullptr;
  page_.Reset();
  ui::ElementList scrollers;
  document_->GetElementsByClassName(scrollers, kPageScrollClass);
  for (ui::Element* scroller : scrollers) {
    if (ui::Element* host = PageHost(scroller)) {
      SetClassIfChanged(host, kPageScrolledClass, NavBarCollapseModel::ContentUnderBar(FromHomeDp(scroller, false)));
    }
  }
  ui::Element* overlay = document_->GetElementById(kChatOverlayId);
  overlay_ = overlay ? overlay->GetObserverPtr() : ui::ObserverPtr<ui::Element>{};
  if (!overlay) {
    chat_.Reset();
    return;
  }
  // A remount (a sheet opening over the chat) rebuilds the transcript, whose scroll position comes back only later:
  // keep the state it had and let the next scroll decide. A chat that closed starts the next one with its bar shown.
  SetClassIfChanged(overlay, kChatHiddenClass, chat_.hidden);
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

ui::Element* ShellNavBarScroll::PageHost(ui::Element* scroller) {
  ui::Element* host = scroller->GetParentNode();
  while (host && !host->IsClassSet(kPageHostClass)) {
    host = host->GetParentNode();
  }
  return host; // none: a page sheet's copy of the page, which keeps its own title row
}

void ShellNavBarScroll::OnPageScroll(ui::Element* scroller) {
  ui::Element* host = PageHost(scroller);
  if (!host) {
    return;
  }
  const float from_top_dp = FromHomeDp(scroller, false);
  SetClassIfChanged(host, kPageScrolledClass, NavBarCollapseModel::ContentUnderBar(from_top_dp));
  // One page scrolls at a time: another page's scroll starts its own run from where that page's bar stands (a shown
  // bar counts from the top: a quick first move already lands well down the page).
  if (page_host_.get() != host) {
    page_host_ = host->GetObserverPtr();
    page_.Reset();
    page_.hidden = host->IsClassSet(kPageHiddenClass);
    page_.turn_dp = page_.hidden ? from_top_dp : 0.f;
  }
  if (page_.OnScroll(from_top_dp)) {
    host->SetClass(kPageHiddenClass, page_.hidden);
  }
}

void ShellNavBarScroll::OnChatScroll(ui::Element* transcript) {
  // Only the phone's chat page has the overlay (found once per remount); the desktop transcript keeps its header.
  ui::Element* overlay = overlay_.get();
  if (!overlay) {
    return;
  }
  // The chat's search field is in the bar: while it is open the bar stays (a search steps through the history).
  ui::Element* panel = transcript->GetParentNode();
  while (panel && !panel->IsClassSet("chat-panel")) {
    panel = panel->GetParentNode();
  }
  if (panel && panel->IsClassSet("chat-panel--searching")) {
    chat_.Reset();
    SetClassIfChanged(overlay, kChatHiddenClass, false);
    return;
  }
  if (chat_.OnScroll(FromHomeDp(transcript, true))) {
    overlay->SetClass(kChatHiddenClass, chat_.hidden);
  }
}

} // namespace pbr
