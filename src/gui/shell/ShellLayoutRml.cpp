#include "gui/shell/ShellLayoutRml.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/ui/ShellInterruption.h"
#include "lib/ui/ShellLayout.h"

#include <sstream>
#include <utility>

namespace pbr {

namespace {

std::string SurfaceChromeClass(CompactChromeFrostSurface surface, CompactChromeFrostSurface active,
                               bool frost_enabled, bool solid) {
  std::string cls = "surface-chrome";
  if (frost_enabled && surface != CompactChromeFrostSurface::None && surface == active) {
    cls += " surface-chrome--frost";
  }
  if (solid) {
    cls += " surface-chrome--solid";
  }
  return cls;
}

} // namespace

ShellLayoutRml::ShellLayoutRml(const ShellState& state, const bool bottom_chrome_open, std::string bottom_chrome_key)
    : state_(state), bottom_chrome_open_(bottom_chrome_open), bottom_chrome_key_(std::move(bottom_chrome_key)) {}

const PaneState* ShellLayoutRml::FindPane(const std::string& key) const {
  for (const PaneState& pane : state_.panes) {
    if (pane.spec.key == key) {
      return &pane;
    }
  }
  return nullptr;
}

bool ShellLayoutRml::ChromeFrostEnabled() const {
  return !state_.reduce_transparency && state_.compact_chrome_frost;
}

std::string ShellLayoutRml::PaneSlot(const std::string& key, const char* extra_class, bool with_composer_slot,
                                     int flex_width_dp) const {
  std::ostringstream out;
  out << "<div class=\"shell-pane shell-pane-" << key;
  if (extra_class && extra_class[0] != '\0') {
    out << ' ' << extra_class;
  }
  out << "\" id=\"pane-" << key << "\"";
  if (flex_width_dp > 0) {
    out << " style=\"flex: 0 1 " << flex_width_dp << "dp;\"";
  }
  out << ">";
  out << "<div class=\"shell-pane-body\" id=\"pane-body-" << key << "\"></div>";
  if (with_composer_slot) {
    out << "<div class=\"shell-pane-composer\" id=\"pane-composer-" << key << "\"></div>";
  }
  out << "</div>";
  return out.str();
}

std::string ShellLayoutRml::ExpandedBase() const {
  std::ostringstream out;
  out << "<div class=\"shell-layer shell-layer-base\" data-model=\"window\">";
  out << "<div class=\"shell-pane-row\">";
  out << "<div class=\"shell-nav-rail\" id=\"shell-nav-rail-mount\"></div>";
  if (ShellLayout::TabHasSecondary(state_.nav_tab)) {
    const char* nav_content = ShellLayout::NavContentKey(state_.nav_tab);
    const bool collapsed = state_.sidebar_collapsed;
    out << "<div class=\"shell-pane shell-pane-secondary";
    if (collapsed) {
      out << " shell-pane-secondary--collapsed";
    }
    out << "\" id=\"shell-nav-content-mount\" style=\"flex: 0 1 " << state_.sidebar_width_dp << "dp;\">";
    out << "<div class=\"shell-pane-body\" id=\"pane-body-" << nav_content << "\"></div>";
    out << "</div>";
    // Drag handle on the pane's right edge. Collapse / expand lives at the bottom of the nav rail.
    if (!collapsed) {
      out << "<div class=\"shell-splitter\" id=\"shell-pane-splitter\"></div>";
    }
  }
  ui::String primary_key = state_.primary_pane_key;
  if (primary_key.empty() && state_.nav_tab == NavTab::Home) {
    primary_key = "home";
  }
  if (!primary_key.empty()) {
    if (const PaneState* pane = FindPane(primary_key.c_str())) {
      out << PaneSlot(pane->spec.key, "shell-pane-primary", pane->spec.provides_composer);
    }
  }
  if (state_.auxiliary_open) {
    for (const PaneState& pane : state_.panes) {
      if (pane.spec.role == PaneRole::Auxiliary) {
        // Drag handle on the pane's left edge.
        out << "<div class=\"shell-splitter\" id=\"shell-aux-splitter\"></div>";
        out << PaneSlot(pane.spec.key, "shell-pane-auxiliary", false, state_.auxiliary_width_dp);
      }
    }
  }
  out << "</div></div>";
  out << AccountSheet();
  return out.str();
}

std::string ShellLayoutRml::AccountSheet() const {
  if (!state_.account_sheet_open) {
    return {};
  }
  const CompactChromeFrostSurface frost = ShellInterruption::ResolveFrostSurface(state_);
  const bool frost_enabled = ChromeFrostEnabled();
  const bool solid = state_.reduce_transparency;
  std::ostringstream out;
  out << "<div class=\"shell-account-sheet-scrim\" data-event-click=\"close_account_sheet()\"></div>";
  out << "<div class=\"shell-account-sheet surface-chrome";
  if (solid) {
    out << " surface-chrome--solid";
  }
  out << "\" id=\"shell-account-sheet\">";
  out << "<div class=\"shell-account-sheet-grabber\"></div>";
  out << "<div class=\"shell-account-sheet-header row "
      << SurfaceChromeClass(CompactChromeFrostSurface::AccountSheetHeader, frost, frost_enabled, solid) << "\">";
  out << "<h2 class=\"heading-2\">Me</h2>";
  out << "<button class=\"shell-close-btn\" type=\"button\" data-event-click=\"close_account_sheet()\">";
  out << "<svg src=\"../icons/close.svg\" width=\"14\" height=\"14\" crop-to-content=\"true\"></svg>";
  out << "</button>";
  out << "</div>";
  out << "<div class=\"shell-pane-body shell-account-sheet-body\" id=\"pane-body-settings\"></div>";
  out << "</div>";
  return out.str();
}

std::string ShellLayoutRml::CompactBase() const {
  const CompactChromeFrostSurface frost = ShellInterruption::ResolveFrostSurface(state_);
  const bool frost_enabled = ChromeFrostEnabled();
  const bool solid = state_.reduce_transparency;
  std::ostringstream out;
  out << "<div class=\"shell-layer shell-layer-base shell-layer-compact\" data-model=\"window\">";

  if (!state_.compact_chat_open) {
    const bool home_inline = state_.nav_tab == NavTab::Home;
    out << "<div class=\"shell-nav-page";
    if (home_inline) {
      out << " shell-nav-page--home";
    }
    out << "\" id=\"shell-nav-page\">";
    if (const char* nav_content = ShellLayout::NavContentKey(state_.nav_tab)) {
      out << "<div class=\"shell-pane-body\" id=\"pane-body-" << nav_content << "\"></div>";
    } else if (home_inline) {
      out << "<div class=\"shell-pane-body\" id=\"pane-body-home\"></div>";
    }
    out << "</div>";
  } else if (state_.nav_tab == NavTab::Sessions) {
    out << "<div class=\"shell-nav-page shell-nav-page--under-overlay\" id=\"shell-nav-page\">";
    out << "<div class=\"shell-pane-body\" id=\"pane-body-sidebar\"></div>";
    out << "</div>";
  }

  if (state_.compact_chat_open) {
    out << "<div class=\"shell-chat-overlay\" id=\"shell-chat-overlay\">";
    out << "<div class=\"shell-chat-overlay-chrome row "
        << SurfaceChromeClass(CompactChromeFrostSurface::ChatOverlayHeader, frost, frost_enabled, solid) << "\">";
    out << "<button class=\"shell-back-btn\" type=\"button\" data-event-click=\"compact_chat_back()\">";
    out << "<svg src=\"../icons/back.svg\"></svg>";
    out << "</button>";
    out << "</div>";
    out << "<div class=\"shell-pane-body\" id=\"pane-body-chat\"></div>";
    out << "<div class=\"shell-composer-mount surface-chrome";
    if (solid) {
      out << " surface-chrome--solid";
    }
    out << "\" id=\"shell-composer-mount\"></div>";
    out << "</div>";
  }

  if (state_.auxiliary_open) {
    out << "<div class=\"shell-sheet-scrim\" data-event-click=\"toggle_auxiliary()\"></div>";
    out << "<div class=\"shell-sheet shell-sheet-auxiliary shell-sheet-compact surface-chrome";
    if (solid) {
      out << " surface-chrome--solid";
    }
    out << "\" id=\"shell-auxiliary-sheet\">";
    out << "<div class=\"shell-sheet-compact-chrome "
        << SurfaceChromeClass(CompactChromeFrostSurface::AuxiliarySheetChrome, frost, frost_enabled, solid)
        << "\"></div>";
    for (const PaneState& pane : state_.panes) {
      if (pane.spec.role == PaneRole::Auxiliary) {
        out << PaneSlot(pane.spec.key, nullptr);
      }
    }
    out << "</div>";
  }

  if (!state_.compact_chat_open) {
    out << "<div class=\"shell-bottom-chrome";
    if (frost == CompactChromeFrostSurface::BottomNav && frost_enabled) {
      out << " shell-bottom-chrome--frost";
    }
    if (solid) {
      out << " shell-bottom-chrome--solid";
    }
    out << "\" id=\"shell-bottom-chrome\">";
    out << "<div class=\"shell-nav-rail shell-nav-rail--compact\" id=\"shell-nav-rail-mount\"></div>";
    out << "</div>";
  }

  out << AccountSheet();
  out << "</div>";
  return out.str();
}

std::string ShellLayoutRml::TransientLayer() const {
  if (state_.transient_stack.empty()) {
    return {};
  }
  const PaneState& top = state_.transient_stack.back();
  const CompactChromeFrostSurface frost = ShellInterruption::ResolveFrostSurface(state_);
  const bool frost_enabled = ChromeFrostEnabled();
  const bool solid = state_.reduce_transparency;
  std::ostringstream out;
  out << "<div class=\"shell-layer shell-layer-transient\" id=\"shell-transient-layer\" data-model=\"window\">";
  out << "<div class=\"shell-transient-chrome "
      << SurfaceChromeClass(CompactChromeFrostSurface::TransientHeader, frost, frost_enabled, solid) << "\">";
  out << "<button class=\"shell-back-btn\" type=\"button\" data-event-click=\"transient_back()\">"
         "<svg src=\"../icons/back.svg\"></svg>"
         "</button>";
  out << "</div>";
  out << PaneSlot(top.spec.key, "shell-pane-transient");
  out << "</div>";
  return out.str();
}

std::string ShellLayoutRml::Overlays() const {
  std::ostringstream out;
  for (const OverlayEntry& overlay : state_.overlay_stack) {
    out << "<div class=\"shell-layer shell-layer-overlay\" data-model=\"window\">";
    out << "<div class=\"shell-scrim\" data-event-click=\"close_layer(" << overlay.id << ")\"></div>";
    out << "<div class=\"shell-frame\">";
    out << "<button class=\"shell-close-btn\" type=\"button\" data-event-click=\"close_layer("
        << overlay.id << ")\">";
    out << "<svg src=\"../icons/close.svg\" width=\"16\" height=\"16\" crop-to-content=\"true\"></svg>";
    out << "</button>";
    out << "<div class=\"shell-overlay-body\" id=\"overlay-body-" << overlay.id << "\"></div>";
    out << "</div></div>";
  }
  return out.str();
}

std::string ShellLayoutRml::BottomChrome() const {
  if (!bottom_chrome_open_ || bottom_chrome_key_.empty()) {
    return {};
  }
  std::ostringstream out;
  // No scrim: IME-replacement chrome; chat stays interactive (back / toggle dismiss).
  out << "<div class=\"shell-layer shell-layer-emoji-keyboard\" data-model=\"window\">";
  out << "<div class=\"shell-emoji-keyboard-panel surface-chrome";
  if (state_.reduce_transparency) {
    out << " surface-chrome--solid";
  }
  out << "\" id=\"shell-emoji-keyboard-panel\">";
  out << "<div class=\"shell-emoji-keyboard-body\" id=\"shell-bottom-chrome-body\"></div>";
  out << "</div></div>";
  return out.str();
}

std::string ShellLayoutRml::Dialog() const {
  if (!state_.dialog.active) {
    return {};
  }
  std::ostringstream out;
  out << "<div class=\"shell-layer shell-layer-dialog\" data-model=\"window\">";
  if (state_.dialog.show_cancel) {
    out << "<div class=\"shell-scrim\" data-event-click=\"dialog_cancel()\"></div>";
  } else {
    out << "<div class=\"shell-scrim\" data-event-click=\"dialog_ok()\"></div>";
  }
  out << "<div class=\"shell-dialog\">";
  out << "<h2 class=\"heading-2 shell-dialog-title\">{{dialog_title}}</h2>";
  out << "<p class=\"text shell-dialog-message\">{{dialog_message}}</p>";
  out << "<label class=\"shell-dialog-checkbox row\" data-if=\"dialog_show_checkbox\">";
  out << "<input type=\"checkbox\" data-value=\"dialog_checkbox_checked\" "
         "data-event-change=\"dialog_toggle_checkbox()\"/>";
  out << "<span>{{dialog_checkbox_label}}</span>";
  out << "</label>";
  out << "<input class=\"shell-dialog-prompt\" type=\"text\" data-if=\"dialog_show_prompt\" "
         "data-value=\"dialog_prompt_value\"/>";
  out << "<div class=\"shell-dialog-actions row\">";
  out << "<button class=\"btn btn-secondary\" data-if=\"dialog_show_cancel\" "
         "data-event-click=\"dialog_cancel()\">"
      << Tr("common.cancel") << "</button>";
  out << "<button class=\"btn btn-primary\" data-event-click=\"dialog_ok()\">"
      << (state_.dialog.ok_label.empty() ? Tr("common.ok") : std::string(state_.dialog.ok_label.c_str()))
      << "</button>";
  out << "</div></div></div>";
  return out.str();
}

std::string ShellLayoutRml::PinGate() const {
  if (!state_.pin_gate.active) {
    return {};
  }
  std::ostringstream out;
  out << "<div class=\"shell-layer shell-layer-dialog\" data-model=\"window\">";
  out << "<div class=\"shell-scrim\"></div>";
  out << "<div class=\"shell-dialog shell-pin-gate\">";
  out << "<h2 class=\"heading-2 shell-dialog-title\">{{pin_gate_title}}</h2>";
  out << "<p class=\"text shell-dialog-message\">{{pin_gate_message}}</p>";
  out << "<p class=\"text shell-pin-gate-error\">{{pin_gate_error}}</p>";
  out << "<div class=\"shell-pin-gate-chooser\" data-if=\"pin_gate_identity_fork_mode\">";
  out << "<button class=\"btn btn-primary\" data-if=\"pin_gate_restore_label != ''\" "
         "data-event-click=\"pin_gate_identity_restore()\">{{pin_gate_restore_label}}</button>";
  out << "<button class=\"btn btn-primary\" data-event-click=\"pin_gate_identity_new()\">"
      << Tr("pin.identity_new") << "</button>";
  out << "<button class=\"btn btn-secondary\" data-event-click=\"pin_gate_identity_link()\">"
      << Tr("pin.identity_link") << "</button>";
  out << "</div>";
  out << "<div class=\"shell-pin-gate-chooser\" data-if=\"pin_gate_chooser_mode\">";
  out << "<button class=\"btn btn-primary\" data-event-click=\"pin_gate_set_pin()\">" << Tr("pin.set_pin")
      << "</button>";
  out << "<button class=\"btn btn-secondary\" data-event-click=\"pin_gate_use_default()\">"
      << Tr("pin.just_continue") << "</button>";
  out << "<button class=\"btn btn-ghost\" data-event-click=\"pin_gate_cancel()\">" << Tr("common.back")
      << "</button>";
  out << "</div>";
  out << "<textarea class=\"field shell-pin-gate-input\" data-if=\"pin_gate_link_paste_mode\" "
         "data-value=\"pin_gate_link_payload\" placeholder=\""
      << Tr("pin.link_paste_placeholder") << "\"></textarea>";
  out << "<input class=\"field shell-pin-gate-input\" type=\"password\" "
         "data-if=\"!pin_gate_chooser_mode && !pin_gate_identity_fork_mode && !pin_gate_link_paste_mode\" "
         "data-value=\"pin_gate_pin\" placeholder=\""
      << Tr("pin.placeholder") << "\"/>";
  out << "<input class=\"field shell-pin-gate-input\" type=\"password\" "
         "data-if=\"pin_gate_create_mode && !pin_gate_chooser_mode\" data-value=\"pin_gate_pin_confirm\" "
         "placeholder=\""
      << Tr("pin.confirm_placeholder") << "\"/>";
  out << "<div class=\"shell-dialog-actions row\" "
         "data-if=\"!pin_gate_chooser_mode && !pin_gate_identity_fork_mode\">";
  out << "<button class=\"btn btn-secondary\" data-if=\"pin_gate_create_mode\" "
         "data-event-click=\"pin_gate_cancel()\">"
      << Tr("pin.not_now") << "</button>";
  out << "<button class=\"btn btn-secondary\" data-if=\"pin_gate_link_paste_mode || pin_gate_restore_pin_mode\" "
         "data-event-click=\"pin_gate_cancel()\">"
      << Tr("common.back") << "</button>";
  out << "<button class=\"btn btn-primary\" data-event-click=\"pin_gate_submit()\">" << Tr("common.continue")
      << "</button>";
  out << "</div>";
  out << "<button class=\"btn btn-ghost\" data-if=\"pin_gate_unlock_mode\" "
         "data-event-click=\"pin_gate_forgot_toggle()\">"
      << Tr("pin.forgot_link") << "</button>";
  out << "<p class=\"text muted shell-pin-gate-note\" data-if=\"pin_gate_unlock_mode && pin_gate_forgot_open\">"
      << Tr("pin.forgot_explain") << "</p>";
  out << "</div></div>";
  return out.str();
}

std::string ShellLayoutRml::ShellRoot() const {
  std::ostringstream out;
  if (state_.layout_mode == LayoutMode::Expanded) {
    out << ExpandedBase();
  } else {
    out << CompactBase();
  }
  out << TransientLayer();
  out << Overlays();
  out << Dialog();
  // Overlay chrome mounts stay empty here; Remount* fills them (avoids full-shell remount).
  out << "<div id=\"shell-dialog-mount\" class=\"shell-overlay-chrome-mount shell-overlay-chrome-mount--dialog\"></div>";
  out << "<div id=\"shell-call-in-progress-mount\" class=\"shell-call-chrome-mount shell-call-chrome-mount--bar\"></div>";
  out << "<div id=\"shell-call-ring-mount\" class=\"shell-call-chrome-mount shell-call-chrome-mount--ring\"></div>";
  out << "<div id=\"shell-pin-gate-mount\" class=\"shell-overlay-chrome-mount shell-overlay-chrome-mount--pin\"></div>";
  out << "<div id=\"shell-onboarding-mount\" class=\"shell-overlay-chrome-mount shell-overlay-chrome-mount--pin\"></div>";
  return out.str();
}

} // namespace pbr
