#pragma once

#include "lib/ui/ShellTypes.h"

namespace pbr {

struct ShellLayout {
  static LayoutMode FromWidth(float width_dp, float breakpoint = ShellConfig{}.compact_breakpoint_dp);
  static const char* LayoutModeString(LayoutMode mode);
  static const char* NavTabString(NavTab tab);
  static void SyncLayoutModeString(ShellState& state);
  static void SyncNavTabString(ShellState& state);
  static PaneVisibility WhichPanesVisible(const ShellState& state);
  static const char* NavContentKey(NavTab tab);
  static bool TabHasSecondary(NavTab tab);
  /** Phones: Me and Contacts are tabs with no button in the tab bar (DEV/PLAN-minimal-nav.md A). They are entered
   *  from an avatar or an invite, show no tab bar, and their root page's back returns to the tab they came from. */
  static bool IsHiddenTabOnCompact(NavTab tab);
  /** Phone layout on a hidden tab (IsHiddenTabOnCompact): its page covers a chat overlay left open under it. */
  static bool HiddenTabShown(const ShellState& state);
  /** Pages that may be pushed (transient) from Settings and keep the tab bar under them (TODO #200). */
  static bool IsSettingsFamilyTransient(const std::string& key);
  /** Phone layout on Settings (Me) with only settings-family pages pushed over it and no secret page: the tab bar is
   *  drawn, over the pushed pages too. Any other pushed page (a chat, the scanner, the PIN gate...) keeps today's none. */
  static bool SettingsTabBarShown(const ShellState& state);
  /** Height of the phone tab bar now: 0 where it is not drawn (a hidden tab, the chat overlay), else `bar_height_dp`
   *  (also under a pushed page, which covers it, and under the AI drawer's scrim). 0 on desktop. */
  static float CompactTabBarDp(const ShellState& state, float bar_height_dp = ShellConfig{}.compact_nav_height_dp);

  static constexpr int kSidebarMinWidthDp = 200;
  static constexpr int kSidebarMaxWidthDp = 480;
  static constexpr int kSidebarDefaultWidthDp = 240;
  static int ClampSidebarWidthDp(int width_dp);
  static constexpr int kAuxiliaryMinWidthDp = 280;
  static constexpr int kAuxiliaryMaxWidthDp = 640;
  static constexpr int kAuxiliaryDefaultWidthDp = 320;
  static int ClampAuxiliaryWidthDp(int width_dp);
  /** Expanded layout, tab has a secondary pane, and the user has not collapsed it. */
  static bool SecondaryPaneShown(const ShellState& state);
  /** Collapsed flag after the user picks `tab` in the nav rail: tabs with a secondary pane re-expand it. */
  static bool SidebarCollapsedAfterNavSelect(bool collapsed, NavTab tab);
  /** A chat picked on the phone's Home (the "New messages" widget) opens from the Chats tab, as if picked in the Chats
   *  list: the chat overlay only opens over Chats, and its back lands on that list. */
  static bool OpenChatSwitchesToChats(LayoutMode mode, NavTab tab);
  /** B166/B167: a paste too big to render freezes the RmlUi textarea, so it is kept in the model but hidden. A
   *  transfer code (the !phrase flow) is always hidden once pasted; the phrase box hides only an over-long paste and
   *  keeps a normal 24-word recovery phrase visible. `payload_len` is the pasted length in bytes. */
  static bool PinGatePayloadTooBigToRender(bool phrase_flow, size_t payload_len);
  /** A bottom inset at or above this is the on-screen keyboard (not the home indicator alone). */
  static constexpr int kImeLatchMinDp = 120;
  static CompactChromeLayout ComputeCompactChromeLayout(const ShellConfig& config,
                                                        int safe_area_top_dp,
                                                        int safe_area_bottom_dp,
                                                        float titlebar_height_dp = 0.f,
                                                        CompactBottomSurface bottom_surface = CompactBottomSurface::None,
                                                        bool bottom_chrome_open = false);
};

} // namespace pbr
