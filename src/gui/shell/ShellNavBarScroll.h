#pragma once

#include "gui/shell/NavBarCollapseModel.h"

#include <ui/base/ObserverPtr.h>
#include <ui/dom/EventListener.h>

namespace ui {
class Element;
class ElementDocument;
class Event;
}

namespace pbr {

/**
 * Phone collapsing bars (NavBarCollapseModel): one bubbling scroll listener on the shell document. The chat
 * transcript's scrolls collapse / expand the chat header (class `chat-bar-compact` on #shell-chat-overlay). It only
 * toggles a class when the state flips; RCSS transitions run the change (transform and opacity, no layout).
 */
class ShellNavBarScroll : public ui::EventListener {
public:
  ~ShellNavBarScroll() override { Detach(); }

  void Attach(ui::ElementDocument* document);
  void Detach();
  /** After a remount: reads the scrollers again and puts the classes back on the new elements. */
  void Refresh();

  void ProcessEvent(ui::Event& event) override;

private:
  void OnChatScroll(ui::Element* transcript);

  ui::ObserverPtr<ui::Element> document_;
  NavBarCollapseModel::ChatHeader chat_;
};

} // namespace pbr
