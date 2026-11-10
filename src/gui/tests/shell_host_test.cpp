#include "gui/shell/ShellFeedback.h"
#include "gui/shell/ShellHost.h"
#include "gui/shell/ShellLayoutRml.h"
#include "lib/ui/ShellInterruption.h"
#include "lib/ui/ShellLayout.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "lib/ui/ShellTypes.h"

#include <gtest/gtest.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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
  overlay_state.overlay_stack.push_back({1, OverlayKind::Generic, "dialog", "views/dialog.rml", {}});
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

  ShellState compact_me_tab{};
  compact_me_tab.layout_mode = LayoutMode::Compact;
  compact_me_tab.nav_tab = NavTab::Me;
  EXPECT_EQ(ShellInterruption::Top(compact_me_tab), InterruptionKind::None);
  EXPECT_FALSE(ShellInterruption::DismissTop(compact_me_tab));

  // A chat overlay left open under a phone hidden tab (Me, Contacts) is not the top: the tab's page is.
  ShellState chat_under_me = chat_overlay_state;
  chat_under_me.nav_tab = NavTab::Me;
  EXPECT_EQ(ShellInterruption::Top(chat_under_me), InterruptionKind::None);
  const PaneVisibility under_me_vis = ShellLayout::WhichPanesVisible(chat_under_me);
  EXPECT_TRUE(under_me_vis.compact_nav_page);
  EXPECT_FALSE(under_me_vis.compact_chat_overlay);
  EXPECT_FALSE(ProjectShellChromeSnapshot(chat_under_me).compact_chat_open) << "not on screen";

  ShellState dismiss_transient = transient_state;
  EXPECT_TRUE(ShellInterruption::DismissTop(dismiss_transient));
  EXPECT_TRUE(dismiss_transient.transient_stack.empty());
  EXPECT_FALSE(dismiss_transient.transient_active);

  ShellState compact_base{};
  compact_base.layout_mode = LayoutMode::Compact;
  EXPECT_EQ(ShellInterruption::ResolveFrostSurface(compact_base), CompactChromeFrostSurface::BottomNav);

  ShellState compact_chat_frost = compact_base;
  compact_chat_frost.compact_chat_open = true;
  EXPECT_EQ(ShellInterruption::ResolveFrostSurface(compact_chat_frost), CompactChromeFrostSurface::ChatOverlayHeader);

  ShellState compact_aux_frost = compact_base;
  compact_aux_frost.auxiliary_open = true;
  EXPECT_EQ(ShellInterruption::ResolveFrostSurface(compact_aux_frost), CompactChromeFrostSurface::AuxiliarySheetChrome);

  ShellState compact_me_frost = compact_base;
  compact_me_frost.nav_tab = NavTab::Me;
  EXPECT_EQ(ShellInterruption::ResolveFrostSurface(compact_me_frost), CompactChromeFrostSurface::BottomNav);

  ShellState compact_transient_frost = compact_base;
  compact_transient_frost.transient_stack.push_back(transient_pane);
  compact_transient_frost.transient_active = true;
  EXPECT_EQ(ShellInterruption::ResolveFrostSurface(compact_transient_frost),
            CompactChromeFrostSurface::TransientHeader);

  // Two bar positions, one frosted surface each: the chat view blurs its header and its composer, nothing else does.
  EXPECT_EQ(ShellInterruption::ResolveFrost(compact_chat_frost).top, CompactChromeFrostSurface::ChatOverlayHeader);
  EXPECT_EQ(ShellInterruption::ResolveFrost(compact_chat_frost).bottom, CompactChromeFrostSurface::None)
      << "the composer floats on a clear bar: no bottom frost in the chat view";
  EXPECT_EQ(ShellInterruption::ResolveFrost(compact_base).bottom, CompactChromeFrostSurface::BottomNav);
  EXPECT_EQ(ShellInterruption::ResolveFrost(compact_aux_frost).bottom, CompactChromeFrostSurface::None);
  EXPECT_EQ(ShellInterruption::ResolveFrost(compact_transient_frost).bottom, CompactChromeFrostSurface::None)
      << "a pushed page over the chat takes the frost; the composer under it does not blur";
  EXPECT_EQ(ShellInterruption::ResolveFrost(compact_transient_frost).top, CompactChromeFrostSurface::TransientHeader);

  // TODO #200: Settings keeps the tab bar, also under its pushed pages: top = the page's header, bottom = the bar.
  PaneState settings_page;
  settings_page.spec.key = "contact_detail";
  ShellState compact_settings_frost = compact_me_frost;
  compact_settings_frost.transient_stack.push_back(settings_page);
  compact_settings_frost.transient_active = true;
  EXPECT_EQ(ShellInterruption::ResolveFrost(compact_settings_frost).top, CompactChromeFrostSurface::TransientHeader);
  EXPECT_EQ(ShellInterruption::ResolveFrost(compact_settings_frost).bottom, CompactChromeFrostSurface::BottomNav);
  compact_settings_frost.secret_page_open = true;
  EXPECT_EQ(ShellInterruption::ResolveFrost(compact_settings_frost).bottom, CompactChromeFrostSurface::None)
      << "a secret page has no bar";
  ShellState compact_scan_frost = compact_me_frost;
  settings_page.spec.key = "qr_scan";
  compact_scan_frost.transient_stack.push_back(settings_page);
  EXPECT_EQ(ShellInterruption::ResolveFrost(compact_scan_frost).bottom, CompactChromeFrostSurface::None);

  ShellState compact_dialog_frost = compact_base;
  compact_dialog_frost.dialog.active = true;
  EXPECT_EQ(ShellInterruption::ResolveFrostSurface(compact_dialog_frost), CompactChromeFrostSurface::None);

  ShellState expanded_base{};
  expanded_base.layout_mode = LayoutMode::Expanded;
  EXPECT_EQ(ShellInterruption::ResolveFrostSurface(expanded_base), CompactChromeFrostSurface::None);
  ShellState compact_dialog_over_chat = compact_chat_frost;
  compact_dialog_over_chat.dialog.active = true;
  EXPECT_EQ(ShellInterruption::ResolveFrost(compact_dialog_over_chat).top, CompactChromeFrostSurface::None);
  EXPECT_EQ(ShellInterruption::ResolveFrost(compact_dialog_over_chat).bottom, CompactChromeFrostSurface::None);
  EXPECT_EQ(ShellInterruption::ResolveFrost(expanded_base).bottom, CompactChromeFrostSurface::None);

  ShellState toast_state{};
  ShellFeedback::ShowToast(toast_state, "Hello", ToastDuration::Short, 1000.f);
  EXPECT_EQ(toast_state.toasts.size(), 1U);
  ShellFeedback::ExpireToasts(toast_state, 5000.f);
  EXPECT_TRUE(toast_state.toasts.empty());

  // Desktop keeps 3 s (Short) / 6 s (Long); phones: 2 s for both, and a new pill replaces the old one.
  ShellState desk_toasts{};
  ShellFeedback::ShowToast(desk_toasts, "Desk", ToastDuration::Short, 1000.f);
  EXPECT_FLOAT_EQ(desk_toasts.toasts.back().expires_at_ms, 4000.f);
  ShellFeedback::ShowToast(desk_toasts, "Desk long", ToastDuration::Long, 1000.f);
  EXPECT_FLOAT_EQ(desk_toasts.toasts.back().expires_at_ms, 7000.f);
  ShellState phone_toasts{};
  phone_toasts.layout_mode = LayoutMode::Compact;
  ShellFeedback::ShowToast(phone_toasts, "One", ToastDuration::Long, 1000.f);
  EXPECT_FLOAT_EQ(phone_toasts.toasts.back().expires_at_ms, 3000.f);
  ShellFeedback::ShowToast(phone_toasts, "Two", ToastDuration::Short, 2500.f);
  ASSERT_EQ(phone_toasts.toasts.size(), 1U);
  EXPECT_FLOAT_EQ(phone_toasts.toasts.back().expires_at_ms, 4500.f);

  // Phones: an app-level error is the bottom pill, never a top banner.
  ShellFeedback::ShowBanner(phone_toasts, "Invalid API key", 1000.f);
  EXPECT_TRUE(phone_toasts.banner_message.empty());
  EXPECT_STREQ(phone_toasts.toasts.back().message.c_str(), "Invalid API key");
  ShellFeedback::ShowStickyBanner(phone_toasts, "Still wrong");
  // A sticky notice (connecting, client compat, call) stays a persistent banner on phones.
  EXPECT_STREQ(phone_toasts.banner_message.c_str(), "Still wrong");
  EXPECT_STREQ(phone_toasts.toasts.back().message.c_str(), "Invalid API key");
  ShellFeedback::DismissBanner(phone_toasts);
  EXPECT_TRUE(phone_toasts.banner_message.empty()) << "dismiss_banner clears a sticky banner on the phone too";

  ShellFeedback::ShowBanner(toast_state, "Offline");
  EXPECT_FALSE(toast_state.banner_message.empty());
  ShellFeedback::DismissBanner(toast_state);
  EXPECT_TRUE(toast_state.banner_message.empty());

  // Banners take themselves down after kBannerTtlMs (Kenneth 2026-10-19: nobody wants to hunt for the ×),
  // and the idle loop must know when to wake for that.
  ShellFeedback::ShowBanner(toast_state, "Offline", 1000.f);
  EXPECT_FALSE(ShellFeedback::ExpireBanner(toast_state, 1000.f + ShellFeedback::kBannerTtlMs - 1.f));
  EXPECT_FALSE(toast_state.banner_message.empty());
  EXPECT_GT(ShellFeedback::SecondsUntilNextFeedbackExpiry(toast_state, 1000.f), 0.0);
  EXPECT_TRUE(ShellFeedback::ExpireBanner(toast_state, 1000.f + ShellFeedback::kBannerTtlMs));
  EXPECT_TRUE(toast_state.banner_message.empty());
  EXPECT_FALSE(ShellFeedback::ExpireBanner(toast_state, 1e9f));
  EXPECT_LT(ShellFeedback::SecondsUntilNextFeedbackExpiry(toast_state, 1000.f), 0.0);

  const ShellConfig config{};
  const CompactChromeLayout no_inset = ShellLayout::ComputeCompactChromeLayout(config, 0, 0);
  EXPECT_FLOAT_EQ(no_inset.shell_top_dp, 0.f);
  EXPECT_FLOAT_EQ(no_inset.content_top_dp, 0.f);
  EXPECT_FLOAT_EQ(no_inset.shell_bottom_dp, 0.f);
  EXPECT_FLOAT_EQ(no_inset.content_padding_bottom_dp, 48.f);
  EXPECT_FLOAT_EQ(no_inset.chrome_bottom_dp, 0.f);
  EXPECT_FLOAT_EQ(no_inset.sheet_bottom_dp, 48.f);
  EXPECT_FLOAT_EQ(no_inset.chrome_horizontal_inset_dp, 0.f);

  const CompactChromeLayout with_inset = ShellLayout::ComputeCompactChromeLayout(config, 47, 34);
  // Body stays edge-to-edge at the top; content_top carries the status-bar inset.
  EXPECT_FLOAT_EQ(with_inset.shell_top_dp, 0.f);
  EXPECT_FLOAT_EQ(with_inset.content_top_dp, 47.f);
  EXPECT_FLOAT_EQ(with_inset.shell_bottom_dp, 34.f);
  EXPECT_FLOAT_EQ(with_inset.content_padding_bottom_dp, 48.f);
  EXPECT_FLOAT_EQ(with_inset.chrome_bottom_dp, 0.f);
  EXPECT_FLOAT_EQ(with_inset.sheet_bottom_dp, 48.f);

  // Keyboard up with no bottom surface: the body stays full-bleed (0) so its background paints behind the keyboard,
  // and the keyboard inset lifts the content via #shell-root instead of shrinking the body.
  const CompactChromeLayout with_keyboard = ShellLayout::ComputeCompactChromeLayout(config, 47, 336);
  EXPECT_FLOAT_EQ(with_keyboard.shell_top_dp, 0.f);
  EXPECT_FLOAT_EQ(with_keyboard.content_top_dp, 47.f);
  EXPECT_FLOAT_EQ(with_keyboard.shell_bottom_dp, 0.f);
  EXPECT_FLOAT_EQ(with_keyboard.content_bottom_inset_dp, 336.f);
  EXPECT_FLOAT_EQ(with_keyboard.content_padding_bottom_dp, 48.f);

  // Phone: the tab bar paints down through the home-indicator strip (document full-bleed, bar pads by the inset).
  const CompactChromeLayout tab_bar =
      ShellLayout::ComputeCompactChromeLayout(config, 47, 34, 0.f, CompactBottomSurface::TabBar);
  EXPECT_FLOAT_EQ(tab_bar.shell_bottom_dp, 0.f);
  EXPECT_FLOAT_EQ(tab_bar.bottom_surface_padding_dp, 34.f);
  EXPECT_FLOAT_EQ(tab_bar.content_padding_bottom_dp, 48.f + 34.f);
  EXPECT_FLOAT_EQ(tab_bar.sheet_bottom_dp, 48.f + 34.f);
  EXPECT_FLOAT_EQ(tab_bar.chrome_bottom_dp, 0.f);

  const CompactChromeLayout tab_bar_no_inset =
      ShellLayout::ComputeCompactChromeLayout(config, 0, 0, 0.f, CompactBottomSurface::TabBar);
  EXPECT_FLOAT_EQ(tab_bar_no_inset.shell_bottom_dp, 0.f);
  EXPECT_FLOAT_EQ(tab_bar_no_inset.bottom_surface_padding_dp, 0.f);
  EXPECT_FLOAT_EQ(tab_bar_no_inset.content_padding_bottom_dp, 48.f);
  EXPECT_FALSE(tab_bar_no_inset.tab_bar_hidden);

  // Keyboard up over a tab-bar page: the tab bar is hidden (the keyboard covers it) and its height is no longer
  // reserved, so the content sits right above the keyboard instead of squeezed above a floating bar. The body stays
  // full-bleed (shell_bottom 0) so its background paints behind the keyboard; the keyboard inset lifts #shell-root.
  const CompactChromeLayout tab_bar_keyboard =
      ShellLayout::ComputeCompactChromeLayout(config, 47, 336, 0.f, CompactBottomSurface::TabBar);
  EXPECT_FLOAT_EQ(tab_bar_keyboard.shell_bottom_dp, 0.f);
  EXPECT_FLOAT_EQ(tab_bar_keyboard.content_bottom_inset_dp, 336.f);
  EXPECT_FLOAT_EQ(tab_bar_keyboard.bottom_surface_padding_dp, 0.f);
  EXPECT_TRUE(tab_bar_keyboard.tab_bar_hidden);
  EXPECT_FLOAT_EQ(tab_bar_keyboard.content_padding_bottom_dp, 0.f);

  // Emoji / attachment panel open over a tab-bar page (bottom_chrome_open): its own inset is zeroed at the call site,
  // so no keyboard is detected, but the tab bar still hides so the panel sits right under the docked composer.
  const CompactChromeLayout tab_bar_bottom_chrome = ShellLayout::ComputeCompactChromeLayout(
      config, 47, 0, 0.f, CompactBottomSurface::TabBar, /*bottom_chrome_open=*/true);
  EXPECT_TRUE(tab_bar_bottom_chrome.tab_bar_hidden);
  EXPECT_FLOAT_EQ(tab_bar_bottom_chrome.content_padding_bottom_dp, 0.f);

  // Chat overlay (no tab bar): the composer pads by the inset; nothing is added to the nav padding.
  const CompactChromeLayout composer =
      ShellLayout::ComputeCompactChromeLayout(config, 47, 34, 0.f, CompactBottomSurface::Composer);
  EXPECT_FLOAT_EQ(composer.shell_bottom_dp, 0.f);
  EXPECT_FLOAT_EQ(composer.bottom_surface_padding_dp, 34.f);
  EXPECT_FLOAT_EQ(composer.content_padding_bottom_dp, 48.f);
  const CompactChromeLayout composer_keyboard =
      ShellLayout::ComputeCompactChromeLayout(config, 47, 336, 0.f, CompactBottomSurface::Composer);
  EXPECT_FLOAT_EQ(composer_keyboard.shell_bottom_dp, 0.f);
  EXPECT_FLOAT_EQ(composer_keyboard.content_bottom_inset_dp, 336.f);
  EXPECT_FLOAT_EQ(composer_keyboard.bottom_surface_padding_dp, 0.f);

  // Phone toast pill: just above the tab bar / composer / keyboard; none on desktop. With a keyboard up the body no
  // longer shrinks, so the toast adds the keyboard inset to clear it (and a hidden tab bar is not there to clear).
  EXPECT_FLOAT_EQ(with_inset.toast_bottom_dp, 0.f);
  EXPECT_FLOAT_EQ(tab_bar.toast_bottom_dp, 34.f + 48.f + 12.f);
  EXPECT_FLOAT_EQ(tab_bar_keyboard.toast_bottom_dp, 336.f + 12.f);         // tab bar hidden; inset clears the keyboard
  EXPECT_FLOAT_EQ(composer_keyboard.toast_bottom_dp, 336.f + 52.f + 12.f); // above the composer, above the keyboard
  EXPECT_FLOAT_EQ(composer.toast_bottom_dp, 34.f + 52.f + 12.f);
  EXPECT_FLOAT_EQ(
      ShellLayout::ComputeCompactChromeLayout(config, 47, 34, 0.f, CompactBottomSurface::Page).toast_bottom_dp,
      34.f + 12.f);

  const CompactChromeLayout page =
      ShellLayout::ComputeCompactChromeLayout(config, 47, 34, 0.f, CompactBottomSurface::Page);
  EXPECT_FLOAT_EQ(page.shell_bottom_dp, 0.f);
  EXPECT_FLOAT_EQ(page.bottom_surface_padding_dp, 34.f);

  const CompactChromeLayout with_titlebar = ShellLayout::ComputeCompactChromeLayout(config, 0, 0, 36.f);
  EXPECT_FLOAT_EQ(with_titlebar.shell_top_dp, 0.f);
  EXPECT_FLOAT_EQ(with_titlebar.content_top_dp, 36.f);
  EXPECT_FLOAT_EQ(with_titlebar.shell_bottom_dp, 0.f);

  const CompactChromeLayout titlebar_and_safe = ShellLayout::ComputeCompactChromeLayout(config, 47, 34, 36.f);
  EXPECT_FLOAT_EQ(titlebar_and_safe.content_top_dp, 83.f);
  EXPECT_FLOAT_EQ(titlebar_and_safe.shell_bottom_dp, 34.f);
}

TEST(ShellHostTest, ProjectShellChromeSnapshot) {
  using namespace pbr;

  ShellState state{};
  state.layout_mode = LayoutMode::Compact;
  state.nav_tab = NavTab::Me;
  state.primary_pane_key = "settings_detail";

  PaneState detail_pane;
  detail_pane.spec.key = "settings_detail";
  state.transient_stack.push_back(detail_pane);

  const ShellChromeSnapshot snap = ProjectShellChromeSnapshot(state);
  EXPECT_EQ(snap.layout_mode, LayoutMode::Compact);
  EXPECT_EQ(snap.nav_tab, NavTab::Me);
  EXPECT_TRUE(snap.settings_detail_transient);
  EXPECT_TRUE(snap.settings_detail_primary);
}

TEST(ShellHostTest, AuxiliaryWidthClamp) {
  using namespace pbr;

  EXPECT_EQ(ShellLayout::ClampAuxiliaryWidthDp(0), 280);
  EXPECT_EQ(ShellLayout::ClampAuxiliaryWidthDp(279), 280);
  EXPECT_EQ(ShellLayout::ClampAuxiliaryWidthDp(280), 280);
  EXPECT_EQ(ShellLayout::ClampAuxiliaryWidthDp(320), 320);
  EXPECT_EQ(ShellLayout::ClampAuxiliaryWidthDp(640), 640);
  EXPECT_EQ(ShellLayout::ClampAuxiliaryWidthDp(641), 640);
  EXPECT_EQ(ShellLayout::ClampAuxiliaryWidthDp(100000), 640);
  EXPECT_EQ(ShellState{}.auxiliary_width_dp, ShellLayout::kAuxiliaryDefaultWidthDp);
}

TEST(ShellHostTest, SidebarWidthClampAndVisibility) {
  using namespace pbr;

  EXPECT_EQ(ShellLayout::ClampSidebarWidthDp(0), 200);
  EXPECT_EQ(ShellLayout::ClampSidebarWidthDp(199), 200);
  EXPECT_EQ(ShellLayout::ClampSidebarWidthDp(200), 200);
  EXPECT_EQ(ShellLayout::ClampSidebarWidthDp(240), 240);
  EXPECT_EQ(ShellLayout::ClampSidebarWidthDp(480), 480);
  EXPECT_EQ(ShellLayout::ClampSidebarWidthDp(481), 480);
  EXPECT_EQ(ShellLayout::ClampSidebarWidthDp(100000), 480);
  EXPECT_EQ(ShellState{}.sidebar_width_dp, ShellLayout::kSidebarDefaultWidthDp);
  EXPECT_FALSE(ShellState{}.sidebar_collapsed);

  ShellState state{};
  state.layout_mode = LayoutMode::Expanded;
  state.nav_tab = NavTab::Sessions;
  EXPECT_TRUE(ShellLayout::SecondaryPaneShown(state));
  state.sidebar_collapsed = true;
  EXPECT_FALSE(ShellLayout::SecondaryPaneShown(state));
  state.sidebar_collapsed = false;
  state.nav_tab = NavTab::Home;
  EXPECT_FALSE(ShellLayout::SecondaryPaneShown(state));
  state.nav_tab = NavTab::Me;
  EXPECT_TRUE(ShellLayout::SecondaryPaneShown(state));
  state.layout_mode = LayoutMode::Compact;
  EXPECT_FALSE(ShellLayout::SecondaryPaneShown(state));

  // Collapsed lists come back on tabs that have one; Home leaves the flag alone.
  EXPECT_FALSE(ShellLayout::SidebarCollapsedAfterNavSelect(true, NavTab::Sessions));
  EXPECT_FALSE(ShellLayout::SidebarCollapsedAfterNavSelect(true, NavTab::Contacts));
  EXPECT_FALSE(ShellLayout::SidebarCollapsedAfterNavSelect(true, NavTab::Me));
  EXPECT_TRUE(ShellLayout::SidebarCollapsedAfterNavSelect(true, NavTab::Home));
  EXPECT_FALSE(ShellLayout::SidebarCollapsedAfterNavSelect(false, NavTab::Home));
}

TEST(ShellHostTest, AChatPickedOnThePhonesHomeOpensFromChats) {
  // Kenneth 2026-11-04 (PLAN-ui-refresh-impl 62.5): a tap on Home's "New messages" row only made it vanish.
  using namespace pbr;
  EXPECT_TRUE(ShellLayout::OpenChatSwitchesToChats(LayoutMode::Compact, NavTab::Home));
  EXPECT_FALSE(ShellLayout::OpenChatSwitchesToChats(LayoutMode::Compact, NavTab::Sessions));
  EXPECT_FALSE(ShellLayout::OpenChatSwitchesToChats(LayoutMode::Compact, NavTab::Contacts));
  EXPECT_FALSE(ShellLayout::OpenChatSwitchesToChats(LayoutMode::Expanded, NavTab::Home))
      << "the desktop keeps its panes";
}

TEST(ShellHostTest, AnOverLongPasteInThePinBoxIsHiddenNotRendered) {
  // B166 (transfer code) then B167 (a transfer code pasted into the phrase box): rendering a multi-KB value in the
  // RmlUi textarea froze the app on device. Anything too big to render is hidden behind a confirmation instead.
  using namespace pbr;
  // Transfer-code box (!phrase_flow): any non-empty paste is a code; hide it. Empty stays visible.
  EXPECT_FALSE(ShellLayout::PinGatePayloadTooBigToRender(/*phrase_flow=*/false, 0));
  EXPECT_TRUE(ShellLayout::PinGatePayloadTooBigToRender(/*phrase_flow=*/false, 20));
  EXPECT_TRUE(ShellLayout::PinGatePayloadTooBigToRender(/*phrase_flow=*/false, 4096));
  // Phrase box: a typed or short-pasted 24-word phrase (hundreds of bytes) stays visible; a transfer code dropped in
  // by mistake (kilobytes) is hidden.
  EXPECT_FALSE(ShellLayout::PinGatePayloadTooBigToRender(/*phrase_flow=*/true, 0));
  EXPECT_FALSE(ShellLayout::PinGatePayloadTooBigToRender(/*phrase_flow=*/true, 216));
  EXPECT_FALSE(ShellLayout::PinGatePayloadTooBigToRender(/*phrase_flow=*/true, 512));
  EXPECT_TRUE(ShellLayout::PinGatePayloadTooBigToRender(/*phrase_flow=*/true, 4096));
}

namespace {

using namespace pbr;

// RequestSyncLayout queues a UI task that holds the host; the UI queue is not run here, so the hosts made here live
// until the process ends (a freed one would leave that task dangling), kept reachable so leak checkers stay quiet.
ShellHost& KeptHost() {
  static std::vector<std::unique_ptr<ShellHost>> hosts;
  return *hosts.emplace_back(std::make_unique<ShellHost>());
}

ShellHost& PhoneHost(const NavTab tab) {
  ShellHost& host = KeptHost();
  host.State().layout_mode = LayoutMode::Compact;
  ShellLayout::SyncLayoutModeString(host.State());
  host.SelectNavTab(tab);
  return host;
}

// Opens a chat overlay as ChatController does for a chat picked on that tab.
void OpenChat(ShellHost& host) {
  host.RegisterPane({.key = "chat", .rml_path = "views/chat.rml", .provides_composer = true});
  host.SetPrimaryPane("chat");
  host.OpenCompactChat();
}

} // namespace

// DEV/PLAN-minimal-nav.md A: on phones Me and Contacts are hidden tabs; their back returns to the tab they came from.
TEST(ShellHostHiddenTabTest, BackFromSettingsReturnsToTheTabItWasOpenedFrom) {
  ShellHost& from_chats = PhoneHost(NavTab::Sessions);
  from_chats.OpenSettingsFromAvatar();
  EXPECT_EQ(from_chats.State().nav_tab, NavTab::Me);
  EXPECT_EQ(ShellLayout::CompactTabBarDp(from_chats.State()), 48.f) << "Settings keeps the tab bar (TODO #200)";
  from_chats.HiddenTabBack();
  EXPECT_EQ(from_chats.State().nav_tab, NavTab::Sessions);

  ShellHost& from_ai = PhoneHost(NavTab::Home);
  from_ai.SelectNavTab(NavTab::Me);
  from_ai.HiddenTabBack();
  EXPECT_EQ(from_ai.State().nav_tab, NavTab::Home);
}

// TODO #200: a tab in the bar on Settings (or a page pushed from it) leaves the whole stack and shows that tab.
TEST(ShellHostHiddenTabTest, ATabTapOnSettingsLeavesTheWholeSettingsStack) {
  ShellHost& host = PhoneHost(NavTab::Sessions);
  host.OpenSettingsFromAvatar();
  EXPECT_EQ(host.State().nav_bar_tab_str, "sessions") << "the bar highlights the tab Settings was entered from";
  host.PushTransient({.key = "contact_detail"});
  host.PushTransient({.key = "blocked_people"});
  ASSERT_EQ(host.State().transient_stack.size(), 2u);
  EXPECT_EQ(ShellLayout::CompactTabBarDp(host.State()), 48.f);

  host.OnNavTabClicked("home");
  EXPECT_EQ(host.State().nav_tab, NavTab::Home);
  EXPECT_TRUE(host.State().transient_stack.empty());
  EXPECT_EQ(host.State().nav_bar_tab_str, "home");

  // From the AI page: back to the AI page, by the same tap.
  ShellHost& from_ai = PhoneHost(NavTab::Home);
  from_ai.OpenSettingsFromAvatar();
  EXPECT_EQ(from_ai.State().nav_bar_tab_str, "home");
  from_ai.OnNavTabClicked("home");
  EXPECT_EQ(from_ai.State().nav_tab, NavTab::Home);

  // A secret page hides the bar until it closes; leaving Settings drops the flag too.
  ShellHost& secret = PhoneHost(NavTab::Home);
  secret.OpenSettingsFromAvatar();
  secret.SetSecretPageOpen(true);
  EXPECT_EQ(ShellLayout::CompactTabBarDp(secret.State()), 0.f);
  secret.HiddenTabBack();
  EXPECT_FALSE(secret.State().secret_page_open);
}

TEST(ShellHostHiddenTabTest, HiddenToHiddenKeepsTheTabBeforeTheFirst) {
  ShellHost& host = PhoneHost(NavTab::Home);
  host.OpenSettingsFromAvatar();
  host.SelectNavTab(NavTab::Contacts); // an invite opened from Settings
  host.HiddenTabBack();
  EXPECT_EQ(host.State().nav_tab, NavTab::Home) << "the tab before Me, not Me";
  host.HiddenTabBack();
  EXPECT_EQ(host.State().nav_tab, NavTab::Home) << "nothing to go back to on a tab in the bar";
}

TEST(ShellHostHiddenTabTest, AnInviteOverAnOpenChatKeepsTheChatForTheWayBack) {
  ShellHost& host = PhoneHost(NavTab::Home);
  OpenChat(host);
  std::vector<std::pair<NavTab, bool>> changes; // the tab, and whether the chat overlay is still open then
  host.SetOnNavTabChanged([&](const NavTab tab) { changes.emplace_back(tab, host.State().compact_chat_open); });

  // ContactsController::OfferInvite picks Contacts through the navigation port.
  const ShellNavigationPorts ports = MakeShellNavigationPorts(host);
  ports.select_nav_tab(NavTab::Contacts);
  EXPECT_EQ(host.State().nav_tab, NavTab::Contacts);
  EXPECT_TRUE(host.State().compact_chat_open) << "the chat stays open under the page";
  EXPECT_EQ(host.State().primary_pane_key, "chat") << "and still holds its chat";
  EXPECT_FALSE(ports.snapshot().compact_chat_open) << "the Contacts page is what shows";
  const std::string rml = ShellLayoutRml(host.State(), false, "").ShellRoot();
  EXPECT_NE(rml.find("id=\"pane-body-contacts\""), std::string::npos);
  EXPECT_EQ(rml.find("id=\"shell-chat-overlay\""), std::string::npos) << "the page is drawn over the chat";
  EXPECT_EQ(rml.find("id=\"shell-bottom-chrome\""), std::string::npos);

  host.HiddenTabBack();
  EXPECT_EQ(host.State().nav_tab, NavTab::Home);
  EXPECT_TRUE(host.State().compact_chat_open);
  EXPECT_EQ(host.State().primary_pane_key, "chat") << "the same chat";
  EXPECT_TRUE(ports.snapshot().compact_chat_open);
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_TRUE(changes.back().second) << "Home hears the chat is still open (Application keeps it, not the list)";

  // Another tab in the bar closes it as before.
  host.SelectNavTab(NavTab::Sessions);
  EXPECT_FALSE(host.State().compact_chat_open);
}

TEST(ShellHostHiddenTabTest, SettingsOpenedOverAnOpenChatReturnsToIt) {
  // The avatar, and the AI's "open settings" (SettingsController::OpenSection selects Me through the port).
  ShellHost& host = PhoneHost(NavTab::Sessions);
  OpenChat(host);
  MakeShellNavigationPorts(host).select_nav_tab(NavTab::Me);
  EXPECT_TRUE(host.State().compact_chat_open);
  EXPECT_FALSE(ShellLayout::WhichPanesVisible(host.State()).compact_chat_overlay);
  host.HiddenTabBack();
  EXPECT_EQ(host.State().nav_tab, NavTab::Sessions);
  EXPECT_TRUE(host.State().compact_chat_open);
  EXPECT_TRUE(ShellLayout::WhichPanesVisible(host.State()).compact_chat_overlay);

  ShellHost& avatar = PhoneHost(NavTab::Home);
  OpenChat(avatar);
  avatar.OpenSettingsFromAvatar();
  EXPECT_TRUE(avatar.State().compact_chat_open);
  avatar.HiddenTabBack();
  EXPECT_EQ(avatar.State().nav_tab, NavTab::Home);
  EXPECT_TRUE(avatar.State().compact_chat_open);
  EXPECT_EQ(avatar.State().primary_pane_key, "chat");
}

TEST(ShellHostHiddenTabTest, SystemBackOnAHiddenTabRootReturnsToThePreviousTab) {
  ShellHost& host = PhoneHost(NavTab::Sessions);
  host.OpenSettingsFromAvatar();
  // A settings page open: back closes it first, and Settings stays.
  bool page_closed = false;
  host.PushLocalBack("settings_detail", [&page_closed]() { page_closed = true; });
  EXPECT_TRUE(host.HandleDismiss());
  EXPECT_TRUE(page_closed);
  EXPECT_EQ(host.State().nav_tab, NavTab::Me);
  // Its root: back to Chats.
  EXPECT_TRUE(host.HandleDismiss());
  EXPECT_EQ(host.State().nav_tab, NavTab::Sessions);
  // A tab in the bar with nothing open: back is the platform's (minimise), not the shell's.
  EXPECT_FALSE(host.HandleDismiss());

  // On the desktop Me is a plain tab: Escape there does not switch tabs.
  ShellHost& desktop = KeptHost();
  desktop.SelectNavTab(NavTab::Me);
  EXPECT_FALSE(desktop.HandleDismiss());
  EXPECT_EQ(desktop.State().nav_tab, NavTab::Me);
}

TEST(ShellHostHiddenTabTest, SettingsEnteredOnTheDesktopStillGoesBackAfterTheWindowNarrows) {
  ShellHost& host = KeptHost();
  host.SelectNavTab(NavTab::Home);
  host.SelectNavTab(NavTab::Me); // the desktop rail's Me
  host.State().layout_mode = LayoutMode::Compact;
  ShellLayout::SyncLayoutModeString(host.State());
  host.HiddenTabBack();
  EXPECT_EQ(host.State().nav_tab, NavTab::Home) << "the tab Settings was entered from, not a stale one";
}

TEST(ShellHostHiddenTabTest, BackToAChatLeftOpenUnderAHiddenTabSaysItIsShownAgain) {
  ShellHost& host = PhoneHost(NavTab::Home);
  OpenChat(host);
  int shown_again = 0;
  bool open_when_told = false;
  host.SetOnCompactChatShownAgain([&]() {
    ++shown_again;
    open_when_told = ShellLayout::WhichPanesVisible(host.State()).compact_chat_overlay;
  });
  host.OpenSettingsFromAvatar();
  host.SelectNavTab(NavTab::Contacts);
  EXPECT_EQ(shown_again, 0) << "still covered";
  host.HiddenTabBack();
  EXPECT_EQ(shown_again, 1) << "the chat's messages are read now (ChatController::OnCompactChatShownAgain)";
  EXPECT_TRUE(open_when_told);

  ShellHost& no_chat = PhoneHost(NavTab::Sessions);
  int told = 0;
  no_chat.SetOnCompactChatShownAgain([&told]() { ++told; });
  no_chat.OpenSettingsFromAvatar();
  no_chat.HiddenTabBack();
  EXPECT_EQ(told, 0) << "no chat was left open";
}

// The wiring the shell callback needs (ChatController needs the whole app to run): Application passes it on, and the
// chat reads the thread, refreshes it and lands on its newest message, as opening it does.
TEST(ShellHostHiddenTabTest, TheChatShownAgainIsMarkedRead) {
  const auto read = [](const char* relative) {
    std::ifstream in(std::string(PP_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    std::erase(text, '\r'); // a Windows checkout has CRLF line ends
    return text;
  };
  EXPECT_NE(read("src/app/Application.cpp")
                .find("shell.SetOnCompactChatShownAgain([&chat]() { chat.OnCompactChatShownAgain(); });"),
            std::string::npos);
  const std::string chat = read("src/gui/chat/ChatController.cpp");
  const size_t at = chat.find("void ChatController::OnCompactChatShownAgain() {");
  ASSERT_NE(at, std::string::npos);
  const std::string body = chat.substr(at, chat.find("\n}\n", at) - at);
  EXPECT_NE(body.find("presenter_.MarkRead(thread_id);"), std::string::npos);
  EXPECT_NE(body.find("RefreshFromMessaging();"), std::string::npos);
  EXPECT_NE(body.find("scroller_.RequestScrollToLatest();"), std::string::npos);
}

TEST(ShellHostHiddenTabTest, ASecondTapOnTheAiTabDoesNotOpenTheAiList) {
  ShellHost& host = PhoneHost(NavTab::Sessions);
  host.OnNavTabClicked("home");
  EXPECT_EQ(host.State().nav_tab, NavTab::Home);
  EXPECT_FALSE(host.State().ai_drawer_open) << "the first tap only goes to the AI page";
  host.OnNavTabClicked("home");
  EXPECT_FALSE(host.State().ai_drawer_open) << "a second tap stays on the AI page";
  host.OpenAiDrawer();
  EXPECT_TRUE(host.State().ai_drawer_open) << "the drawer still opens from its own entry";
  host.CloseAiDrawer();
  EXPECT_FALSE(host.State().ai_drawer_open);

  OpenChat(host);
  host.OnNavTabClicked("home");
  EXPECT_FALSE(host.State().ai_drawer_open) << "not over an open AI chat";
}

TEST(ShellHostHiddenTabTest, TheAiDrawerFlagNeverSurvivesATabSwitch) {
  for (const NavTab to : {NavTab::Home, NavTab::Sessions, NavTab::Contacts, NavTab::Me}) {
    ShellHost& host = PhoneHost(NavTab::Home);
    host.OpenAiDrawer();
    ASSERT_TRUE(host.State().ai_drawer_open);
    host.SelectNavTab(to);
    EXPECT_FALSE(host.State().ai_drawer_open) << ShellLayout::NavTabString(to);
  }
  ShellHost& cleared = PhoneHost(NavTab::Home);
  cleared.OpenAiDrawer();
  cleared.ClearTabContext();
  EXPECT_FALSE(cleared.State().ai_drawer_open) << "the tab's context goes with it";
}

namespace pbr {

// Reaches ShellHost's private call answer handlers (a friend of the class).
struct ShellHostTestAccess {
  static void Accept(ShellHost& host) { host.CallAccept(); }
  static void AcceptVoice(ShellHost& host) { host.CallAcceptVoice(); }
  static void AcceptCharge(ShellHost& host) { host.CallAcceptCharge(); }
  static void Decline(ShellHost& host) { host.CallDecline(); }
};

namespace {

CallChromeSnapshot Ring(const bool awaiting) {
  CallChromeSnapshot snapshot;
  snapshot.ring.active = true;
  snapshot.ring.awaiting_invite = awaiting;
  return snapshot;
}

bool PinLayerHasOverCall(ShellHost& host) {
  return ShellLayoutRml(host.State(), false, "").PinGate().find("shell-pin-gate--over-call") != std::string::npos;
}

} // namespace

// The unlock layer follows the awaiting page: the class is there while it is up and gone when it goes, whichever came
// first (the layer is remounted on the flip; here the markup it would mount is checked).
TEST(ShellHostCallRingTest, ThePinLayerClassFollowsTheAwaitingFlag) {
  ShellHost& host = PhoneHost(NavTab::Home);
  PinGateState pin;
  pin.active = true;
  pin.unlock_mode = true;
  host.ApplyPinGateState(pin);
  EXPECT_FALSE(PinLayerHasOverCall(host));
  host.ApplyCallChromeSnapshot(Ring(true), CallChromeUpdate::Remount);
  EXPECT_TRUE(host.State().call_ring.awaiting_invite);
  EXPECT_TRUE(PinLayerHasOverCall(host)) << "PIN card was already up when the page came";
  host.ApplyCallChromeSnapshot(Ring(false), CallChromeUpdate::Remount);
  EXPECT_FALSE(PinLayerHasOverCall(host)) << "a real ring: the card is back in the middle";
  host.ApplyCallChromeSnapshot(Ring(true), CallChromeUpdate::Remount);
  host.ApplyCallChromeSnapshot(CallChromeSnapshot{}, CallChromeUpdate::Remount);
  EXPECT_FALSE(PinLayerHasOverCall(host)) << "the page ended while the card was up";
}

TEST(ShellHostCallRingTest, TheAnswersDoNothingWhileAwaitingTheInviteAndActOtherwise) {
  ShellHost& host = PhoneHost(NavTab::Home);
  int accepts = 0;
  int voice = 0;
  int charge = 0;
  int declines = 0;
  CallActionsPorts ports;
  ports.accept_incoming = [&]() { ++accepts; };
  ports.accept_incoming_voice_only = [&]() { ++voice; };
  ports.accept_incoming_with_charge = [&]() { ++charge; };
  ports.decline_incoming = [&]() { ++declines; };
  host.BindCallActions(std::move(ports));

  host.ApplyCallChromeSnapshot(Ring(true), CallChromeUpdate::Remount);
  ShellHostTestAccess::Accept(host);
  ShellHostTestAccess::AcceptVoice(host);
  ShellHostTestAccess::AcceptCharge(host);
  ShellHostTestAccess::Decline(host);
  EXPECT_EQ(accepts + voice + charge + declines, 0);

  host.ApplyCallChromeSnapshot(Ring(false), CallChromeUpdate::DirtyOnly);
  ShellHostTestAccess::Accept(host);
  ShellHostTestAccess::AcceptVoice(host);
  ShellHostTestAccess::AcceptCharge(host);
  ShellHostTestAccess::Decline(host);
  EXPECT_EQ(accepts, 1);
  EXPECT_EQ(voice, 1);
  EXPECT_EQ(charge, 1);
  EXPECT_EQ(declines, 1);
}

TEST(ShellHostCallRingTest, TheStartupCoverIsNotShownOverAnActiveRing) {
  ShellHost& host = KeptHost();
  host.ReconcileStartupCover();
  EXPECT_TRUE(host.State().startup_cover_visible) << "cold start: the logo cover";
  host.ApplyCallChromeSnapshot(Ring(true), CallChromeUpdate::Remount);
  EXPECT_FALSE(host.State().startup_cover_visible);
  host.ApplyCallChromeSnapshot(CallChromeSnapshot{}, CallChromeUpdate::Remount);
}

} // namespace pbr
