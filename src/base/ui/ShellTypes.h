#pragma once

#include <RmlUi/Core/Types.h>

#include <functional>
#include <string>
#include <vector>

namespace pbr {

enum class PaneRole { Primary, Secondary, Auxiliary, Transient };

enum class LayoutMode { Compact, Expanded };

enum class NavTab { Home, Sessions, Contacts, Settings };

enum class OverlayKind { Generic, Alert, Confirm, Custom };

enum class InterruptionKind {
  None,
  CompactChatOverlay,
  AuxiliarySheet,
  Transient,
  OverlayLayer,
  Dialog,
};

enum class ToastDuration { Short, Long };

struct ShellConfig {
  float compact_breakpoint_dp = 768.f;
  float nav_rail_width_dp = 56.f;
  float secondary_width_dp = 240.f;
  float auxiliary_width_dp = 320.f;
  float toolbar_height_dp = 48.f;
  float frame_padding_dp = 24.f;
  size_t max_toasts = 3;
  float toast_short_ms = 3000.f;
};

struct PaneSpec {
  std::string key;
  std::string rml_path;
  PaneRole role = PaneRole::Primary;
  Rml::String toolbar_label;
  bool provides_composer = false;
};

struct PaneState {
  PaneSpec spec;
  int id = 0;
};

struct OverlayEntry {
  int id = 0;
  OverlayKind kind = OverlayKind::Generic;
  std::string rml_path;
  std::function<void(bool)> on_result;
};

struct ToastEntry {
  int id = 0;
  Rml::String message;
  float expires_at_ms = 0.f;
};

struct DialogState {
  bool active = false;
  OverlayKind kind = OverlayKind::Alert;
  Rml::String title;
  Rml::String message;
  bool show_cancel = false;
  std::function<void(bool)> on_result;
};

struct PaneVisibility {
  bool secondary = false;
  bool primary = true;
  bool auxiliary = false;
  bool compact_nav_page = false;
  bool compact_chat_overlay = false;
  bool auxiliary_sheet = false;
};

struct ShellState {
  LayoutMode layout_mode = LayoutMode::Expanded;
  Rml::String layout_mode_str = "expanded";
  NavTab nav_tab = NavTab::Home;
  Rml::String nav_tab_str = "home";
  std::vector<PaneState> panes;
  std::vector<PaneState> transient_stack;
  std::vector<OverlayEntry> overlay_stack;

  bool compact_chat_open = false;
  bool auxiliary_open = false;
  bool auxiliary_available = false;
  bool transient_active = false;

  Rml::String primary_pane_key;

  Rml::String banner_message;
  std::vector<ToastEntry> toasts;
  DialogState dialog;

  bool activity_visible = false;

  float shell_width_dp = 1280.f;
};

} // namespace pbr
