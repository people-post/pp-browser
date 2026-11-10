#pragma once

#include "gui/shell/NavBarCollapseModel.h"

#include <ui/base/ObserverPtr.h>
#include <ui/dom/EventListener.h>

namespace ui {
class Element;
class ElementDocument;
class Event;
} // namespace ui

namespace pbr {

/**
 * Phone bars that hide on scroll (NavBarCollapseModel): one bubbling scroll listener on the shell document. The chat
 * transcript's scrolls hide / show the chat header (class `chat-bar-hidden` on #shell-chat-overlay). A pushed page's
 * scroller marked `nav-bar-scroll` sets, on its `nav-bar-host` (the page holding both its bar and the scroller),
 * `nav-bar-hidden` and `nav-bar-scrolled` (content under the bar: it takes its material). It only toggles a class when
 * a state flips; RCSS transitions run the change (transform and opacity, no layout).
 */
class ShellNavBarScroll : public ui::EventListener {
public:
  ~ShellNavBarScroll() override { Detach(); }

  void Attach(ui::ElementDocument* document);
  void Detach();
  /** After a remount: reads the pages' scrollers again and puts the classes back on the new elements. */
  void Refresh();

  void ProcessEvent(ui::Event& event) override;

private:
  void OnChatScroll(ui::Element* transcript);
  void OnPageScroll(ui::Element* scroller);
  static ui::Element* PageHost(ui::Element* scroller);

  ui::ObserverPtr<ui::Element> document_;
  /** #shell-chat-overlay while a chat is open (phone). */
  ui::ObserverPtr<ui::Element> overlay_;
  NavBarCollapseModel::Bar chat_;
  /** The page whose scroll ran last and its bar. */
  ui::ObserverPtr<ui::Element> page_host_;
  NavBarCollapseModel::Bar page_;
};

} // namespace pbr
