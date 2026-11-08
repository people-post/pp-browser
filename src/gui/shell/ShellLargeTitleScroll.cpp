#include "gui/shell/ShellLargeTitleScroll.h"

#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/Event.h>

namespace pbr {

namespace {

constexpr const char* kScrollerClass = "root-scroll";

} // namespace

bool ShellLargeTitleScroll::CollapsedAt(const float scroll_top_px, const float dp_ratio, const float threshold_dp) {
  return scroll_top_px >= threshold_dp * (dp_ratio > 0.f ? dp_ratio : 1.f);
}

void ShellLargeTitleScroll::Attach(ui::Element* page, Changed on_change) {
  Detach();
  if (!page) {
    return;
  }
  page_ = page->GetObserverPtr();
  on_change_ = std::move(on_change);
  page->AddEventListener(ui::EventId::Scroll, this);
  Refresh();
}

void ShellLargeTitleScroll::Detach() {
  if (page_) {
    page_->RemoveEventListener(ui::EventId::Scroll, this);
  }
  page_ = nullptr;
  on_change_ = nullptr;
  collapsed_ = false;
}

void ShellLargeTitleScroll::Refresh() {
  if (!page_) {
    return;
  }
  ui::ElementList scrollers;
  page_->GetElementsByClassName(scrollers, kScrollerClass);
  Report(scrollers.empty() ? nullptr : scrollers.front());
}

void ShellLargeTitleScroll::ProcessEvent(ui::Event& event) {
  ui::Element* target = event.GetTargetElement();
  if (target && target->IsClassSet(kScrollerClass)) {
    Report(target);
  }
}

void ShellLargeTitleScroll::Report(ui::Element* scroller) {
  float ratio = 1.f;
  float top = 0.f;
  float threshold_dp = kLargeTitleDp;
  if (scroller) {
    threshold_dp = scroller->GetAttribute<float>("collapse-at", kLargeTitleDp);
    if (ui::Context* context = scroller->GetContext()) {
      ratio = context->GetDensityIndependentPixelRatio();
    }
    top = scroller->GetScrollTop();
  }
  const bool collapsed = CollapsedAt(top, ratio, threshold_dp);
  if (collapsed == collapsed_) {
    return;
  }
  collapsed_ = collapsed;
  if (on_change_) {
    on_change_(collapsed);
  }
}

} // namespace pbr
