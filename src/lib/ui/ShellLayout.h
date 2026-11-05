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
                                                        CompactBottomSurface bottom_surface = CompactBottomSurface::None);
};

} // namespace pbr
