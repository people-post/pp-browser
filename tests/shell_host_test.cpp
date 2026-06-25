#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellInterruption.h"
#include "feature/ui/ShellLayout.h"
#include "base/ui/ShellTypes.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
  using namespace pbr;

  assert(ShellLayout::FromWidth(767.f) == LayoutMode::Compact);
  assert(ShellLayout::FromWidth(768.f) == LayoutMode::Expanded);
  assert(ShellLayout::FromWidth(1280.f) == LayoutMode::Expanded);
  assert(ShellLayout::NavTabString(NavTab::Home) == std::string("home"));
  assert(ShellLayout::NavContentKey(NavTab::Home) == nullptr);
  assert(!ShellLayout::TabHasSecondary(NavTab::Home));
  assert(ShellLayout::TabHasSecondary(NavTab::Sessions));
  assert(ShellLayout::NavContentKey(NavTab::Sessions) == std::string("sidebar"));
  assert(ShellLayout::NavContentKey(NavTab::Contacts) == std::string("contacts"));
  assert(ShellLayout::NavContentKey(NavTab::Settings) == std::string("settings"));

  ShellState cleared_tab{};
  cleared_tab.primary_pane_key = "chat";
  cleared_tab.auxiliary_open = true;
  cleared_tab.transient_active = true;
  cleared_tab.compact_chat_open = true;
  cleared_tab.primary_pane_key.clear();
  cleared_tab.auxiliary_open = false;
  cleared_tab.transient_active = false;
  cleared_tab.compact_chat_open = false;
  assert(cleared_tab.primary_pane_key.empty());
  assert(!cleared_tab.auxiliary_open);
  assert(!cleared_tab.transient_active);
  assert(!cleared_tab.compact_chat_open);

  ShellState expanded{};
  expanded.layout_mode = LayoutMode::Expanded;
  expanded.auxiliary_open = true;
  const PaneVisibility expanded_vis = ShellLayout::WhichPanesVisible(expanded);
  assert(expanded_vis.primary);
  assert(expanded_vis.secondary);
  assert(expanded_vis.auxiliary);
  assert(!expanded_vis.compact_nav_page);
  assert(!expanded_vis.compact_chat_overlay);
  assert(!expanded_vis.auxiliary_sheet);

  ShellState compact{};
  compact.layout_mode = LayoutMode::Compact;
  compact.compact_chat_open = true;
  compact.auxiliary_open = true;
  const PaneVisibility compact_vis = ShellLayout::WhichPanesVisible(compact);
  assert(compact_vis.primary);
  assert(!compact_vis.secondary);
  assert(!compact_vis.auxiliary);
  assert(!compact_vis.compact_nav_page);
  assert(compact_vis.compact_chat_overlay);
  assert(compact_vis.auxiliary_sheet);

  ShellState compact_nav{};
  compact_nav.layout_mode = LayoutMode::Compact;
  const PaneVisibility compact_nav_vis = ShellLayout::WhichPanesVisible(compact_nav);
  assert(compact_nav_vis.compact_nav_page);
  assert(!compact_nav_vis.compact_chat_overlay);

  ShellState dialog_state{};
  dialog_state.dialog.active = true;
  assert(ShellInterruption::Top(dialog_state) == InterruptionKind::Dialog);

  ShellState overlay_state{};
  overlay_state.overlay_stack.push_back({1, OverlayKind::Generic, "views/dialog.rml", {}});
  assert(ShellInterruption::Top(overlay_state) == InterruptionKind::OverlayLayer);

  ShellState transient_state{};
  PaneState transient_pane;
  transient_pane.spec.key = "settings";
  transient_pane.id = 1;
  transient_state.transient_stack.push_back(transient_pane);
  transient_state.transient_active = true;
  assert(ShellInterruption::Top(transient_state) == InterruptionKind::Transient);

  ShellState chat_overlay_state{};
  chat_overlay_state.layout_mode = LayoutMode::Compact;
  chat_overlay_state.compact_chat_open = true;
  assert(ShellInterruption::Top(chat_overlay_state) == InterruptionKind::CompactChatOverlay);

  ShellState dismiss_chat = chat_overlay_state;
  assert(ShellInterruption::DismissTop(dismiss_chat));
  assert(!dismiss_chat.compact_chat_open);

  ShellState dismiss_transient = transient_state;
  assert(ShellInterruption::DismissTop(dismiss_transient));
  assert(dismiss_transient.transient_stack.empty());
  assert(!dismiss_transient.transient_active);

  ShellState toast_state{};
  ShellFeedback::ShowToast(toast_state, "Hello", ToastDuration::Short, 1000.f);
  assert(toast_state.toasts.size() == 1);
  ShellFeedback::ExpireToasts(toast_state, 5000.f);
  assert(toast_state.toasts.empty());

  ShellFeedback::ShowBanner(toast_state, "Offline");
  assert(!toast_state.banner_message.empty());
  ShellFeedback::DismissBanner(toast_state);
  assert(toast_state.banner_message.empty());

  std::cout << "shell_host_test ok\n";
  return 0;
}
