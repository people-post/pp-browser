#pragma once

#include <ui/base/ObserverPtr.h>
#include <ui/dom/EventListener.h>

#include <functional>

namespace ui {
class Element;
class Event;
}

namespace pbr {

/**
 * Phone layout: the root pages (Chats, Contacts, Me) open on a 52dp large title that collapses into the nav bar's
 * small title once the page is scrolled past it. One bubbling scroll listener on the nav page watches the scroller
 * marked `root-scroll` (its `collapse-at` attribute, in dp, replaces the 52dp default: the Chats page's header is 104dp
 * with its search box right under it) and reports only when the collapsed state flips (no work per scroll event otherwise).
 */
class ShellLargeTitleScroll : public ui::EventListener {
public:
  static constexpr float kLargeTitleDp = 52.f;

  /** True once the page has scrolled the whole large title away. `dp_ratio` converts layout px to dp. */
  static bool CollapsedAt(float scroll_top_px, float dp_ratio, float threshold_dp = kLargeTitleDp);

  using Changed = std::function<void(bool collapsed)>;

  void Attach(ui::Element* page, Changed on_change);
  void Detach();
  /** Reads the current scroller (after a remount restored its position) and reports if the state differs. */
  void Refresh();

  void ProcessEvent(ui::Event& event) override;

private:
  void Report(ui::Element* scroller);

  ui::ObserverPtr<ui::Element> page_;
  Changed on_change_;
  bool collapsed_ = false;
};

} // namespace pbr
