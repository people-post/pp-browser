#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellInterruption.h"
#include "feature/ui/ShellLayout.h"
#include "base/ui/ShellTypes.h"

#include <gtest/gtest.h>
#include <string>

TEST(ShellHostTest, LayoutInterruptionAndFeedbackBehavior) {
  using namespace pbr;

  EXPECT_EQ(ShellLayout::FromWidth(767.f), LayoutMode::Compact);
  EXPECT_EQ(ShellLayout::FromWidth(768.f), LayoutMode::Expanded);
  EXPECT_EQ(ShellLayout::FromWidth(1280.f), LayoutMode::Expanded);
  EXPECT_EQ(ShellLayout::NavTabString(NavTab::Home), std::string("home"));
  EXPECT_EQ(ShellLayout::NavContentKey(NavTab::Home), nullptr);
  EXPECT_FALSE(ShellLayout::TabHasSecondary(NavTab::Home));
  EXPECT_TRUE(ShellLayout::TabHasSecondary(NavTab::Sessions));
  EXPECT_EQ(ShellLayout::NavContentKey(NavTab::Sessions), std::string("sidebar"));
  EXPECT_EQ(ShellLayout::NavContentKey(NavTab::Contacts), std::string("contacts"));

  ShellState cleared_tab{};
  cleared_tab.primary_pane_key = "chat";
  cleared_tab.auxiliary_open = true;
  cleared_tab.transient_active = true;
  cleared_tab.compact_chat_open = true;
  cleared_tab.primary_pane_key.clear();
  cleared_tab.auxiliary_open = false;
  cleared_tab.transient_active = false;
  cleared_tab.compact_chat_open = false;
  EXPECT_TRUE(cleared_tab.primary_pane_key.empty());
  EXPECT_FALSE(cleared_tab.auxiliary_open);
  EXPECT_FALSE(cleared_tab.transient_active);
  EXPECT_FALSE(cleared_tab.compact_chat_open);

  ShellState expanded{};
  expanded.layout_mode = LayoutMode::Expanded;
  expanded.auxiliary_open = true;
  const PaneVisibility expanded_vis = ShellLayout::WhichPanesVisible(expanded);
  EXPECT_TRUE(expanded_vis.primary);
  EXPECT_TRUE(expanded_vis.secondary);
  EXPECT_TRUE(expanded_vis.auxiliary);
  EXPECT_FALSE(expanded_vis.compact_nav_page);
  EXPECT_FALSE(expanded_vis.compact_chat_overlay);
  EXPECT_FALSE(expanded_vis.auxiliary_sheet);

  ShellState compact{};
  compact.layout_mode = LayoutMode::Compact;
  compact.compact_chat_open = true;
  compact.auxiliary_open = true;
  const PaneVisibility compact_vis = ShellLayout::WhichPanesVisible(compact);
  EXPECT_TRUE(compact_vis.primary);
  EXPECT_FALSE(compact_vis.secondary);
  EXPECT_FALSE(compact_vis.auxiliary);
  EXPECT_FALSE(compact_vis.compact_nav_page);
  EXPECT_TRUE(compact_vis.compact_chat_overlay);
  EXPECT_TRUE(compact_vis.auxiliary_sheet);

  ShellState compact_nav{};
  compact_nav.layout_mode = LayoutMode::Compact;
  const PaneVisibility compact_nav_vis = ShellLayout::WhichPanesVisible(compact_nav);
  EXPECT_TRUE(compact_nav_vis.compact_nav_page);
  EXPECT_FALSE(compact_nav_vis.compact_chat_overlay);

  ShellState dialog_state{};
  dialog_state.dialog.active = true;
  EXPECT_EQ(ShellInterruption::Top(dialog_state), InterruptionKind::Dialog);

  ShellState overlay_state{};
  overlay_state.overlay_stack.push_back({1, OverlayKind::Generic, "views/dialog.rml", {}});
  EXPECT_EQ(ShellInterruption::Top(overlay_state), InterruptionKind::OverlayLayer);

  ShellState transient_state{};
  PaneState transient_pane;
  transient_pane.spec.key = "settings";
  transient_pane.id = 1;
  transient_state.transient_stack.push_back(transient_pane);
  transient_state.transient_active = true;
  EXPECT_EQ(ShellInterruption::Top(transient_state), InterruptionKind::Transient);

  ShellState chat_overlay_state{};
  chat_overlay_state.layout_mode = LayoutMode::Compact;
  chat_overlay_state.compact_chat_open = true;
  EXPECT_EQ(ShellInterruption::Top(chat_overlay_state), InterruptionKind::CompactChatOverlay);

  ShellState dismiss_chat = chat_overlay_state;
  EXPECT_TRUE(ShellInterruption::DismissTop(dismiss_chat));
  EXPECT_FALSE(dismiss_chat.compact_chat_open);

  ShellState account_sheet_state{};
  account_sheet_state.account_sheet_open = true;
  EXPECT_EQ(ShellInterruption::Top(account_sheet_state), InterruptionKind::AccountSheet);

  ShellState dismiss_account_sheet = account_sheet_state;
  EXPECT_TRUE(ShellInterruption::DismissTop(dismiss_account_sheet));
  EXPECT_FALSE(dismiss_account_sheet.account_sheet_open);

  ShellState transient_with_sheet = transient_state;
  transient_with_sheet.account_sheet_open = true;
  EXPECT_EQ(ShellInterruption::Top(transient_with_sheet), InterruptionKind::Transient);

  ShellState dismiss_transient = transient_state;
  EXPECT_TRUE(ShellInterruption::DismissTop(dismiss_transient));
  EXPECT_TRUE(dismiss_transient.transient_stack.empty());
  EXPECT_FALSE(dismiss_transient.transient_active);

  ShellState compact_base{};
  compact_base.layout_mode = LayoutMode::Compact;
  EXPECT_EQ(ShellInterruption::ResolveFrostSurface(compact_base),
            CompactChromeFrostSurface::BottomNav);

  ShellState compact_chat_frost = compact_base;
  compact_chat_frost.compact_chat_open = true;
  EXPECT_EQ(ShellInterruption::ResolveFrostSurface(compact_chat_frost),
            CompactChromeFrostSurface::ChatOverlayHeader);

  ShellState compact_aux_frost = compact_base;
  compact_aux_frost.auxiliary_open = true;
  EXPECT_EQ(ShellInterruption::ResolveFrostSurface(compact_aux_frost),
            CompactChromeFrostSurface::AuxiliarySheetChrome);

  ShellState compact_account_frost = compact_base;
  compact_account_frost.account_sheet_open = true;
  EXPECT_EQ(ShellInterruption::ResolveFrostSurface(compact_account_frost),
            CompactChromeFrostSurface::AccountSheetHeader);

  ShellState compact_transient_frost = compact_base;
  compact_transient_frost.transient_stack.push_back(transient_pane);
  compact_transient_frost.transient_active = true;
  EXPECT_EQ(ShellInterruption::ResolveFrostSurface(compact_transient_frost),
            CompactChromeFrostSurface::TransientHeader);

  ShellState compact_dialog_frost = compact_base;
  compact_dialog_frost.dialog.active = true;
  EXPECT_EQ(ShellInterruption::ResolveFrostSurface(compact_dialog_frost),
            CompactChromeFrostSurface::None);

  ShellState expanded_base{};
  expanded_base.layout_mode = LayoutMode::Expanded;
  EXPECT_EQ(ShellInterruption::ResolveFrostSurface(expanded_base), CompactChromeFrostSurface::None);

  ShellState toast_state{};
  ShellFeedback::ShowToast(toast_state, "Hello", ToastDuration::Short, 1000.f);
  EXPECT_EQ(toast_state.toasts.size(), 1U);
  ShellFeedback::ExpireToasts(toast_state, 5000.f);
  EXPECT_TRUE(toast_state.toasts.empty());

  ShellFeedback::ShowBanner(toast_state, "Offline");
  EXPECT_FALSE(toast_state.banner_message.empty());
  ShellFeedback::DismissBanner(toast_state);
  EXPECT_TRUE(toast_state.banner_message.empty());

  const ShellConfig config{};
  const CompactChromeLayout no_inset = ShellLayout::ComputeCompactChromeLayout(config, 0, 0);
  EXPECT_FLOAT_EQ(no_inset.shell_top_dp, 0.f);
  EXPECT_FLOAT_EQ(no_inset.content_top_dp, 0.f);
  EXPECT_FLOAT_EQ(no_inset.shell_bottom_dp, 0.f);
  EXPECT_FLOAT_EQ(no_inset.content_padding_bottom_dp, 56.f);
  EXPECT_FLOAT_EQ(no_inset.chrome_bottom_dp, 0.f);
  EXPECT_FLOAT_EQ(no_inset.sheet_bottom_dp, 56.f);
  EXPECT_FLOAT_EQ(no_inset.chrome_horizontal_inset_dp, 12.f);

  const CompactChromeLayout with_inset = ShellLayout::ComputeCompactChromeLayout(config, 47, 34);
  // Body stays edge-to-edge at the top; content_top carries the status-bar inset.
  EXPECT_FLOAT_EQ(with_inset.shell_top_dp, 0.f);
  EXPECT_FLOAT_EQ(with_inset.content_top_dp, 47.f);
  EXPECT_FLOAT_EQ(with_inset.shell_bottom_dp, 34.f);
  EXPECT_FLOAT_EQ(with_inset.content_padding_bottom_dp, 56.f);
  EXPECT_FLOAT_EQ(with_inset.chrome_bottom_dp, 0.f);
  EXPECT_FLOAT_EQ(with_inset.sheet_bottom_dp, 56.f);

  const CompactChromeLayout with_keyboard = ShellLayout::ComputeCompactChromeLayout(config, 47, 336);
  EXPECT_FLOAT_EQ(with_keyboard.shell_top_dp, 0.f);
  EXPECT_FLOAT_EQ(with_keyboard.content_top_dp, 47.f);
  EXPECT_FLOAT_EQ(with_keyboard.shell_bottom_dp, 336.f);
  EXPECT_FLOAT_EQ(with_keyboard.content_padding_bottom_dp, 56.f);

  const CompactChromeLayout with_titlebar = ShellLayout::ComputeCompactChromeLayout(config, 0, 0, 36.f);
  EXPECT_FLOAT_EQ(with_titlebar.shell_top_dp, 0.f);
  EXPECT_FLOAT_EQ(with_titlebar.content_top_dp, 36.f);
  EXPECT_FLOAT_EQ(with_titlebar.shell_bottom_dp, 0.f);

  const CompactChromeLayout titlebar_and_safe =
      ShellLayout::ComputeCompactChromeLayout(config, 47, 34, 36.f);
  EXPECT_FLOAT_EQ(titlebar_and_safe.content_top_dp, 83.f);
  EXPECT_FLOAT_EQ(titlebar_and_safe.shell_bottom_dp, 34.f);
}
