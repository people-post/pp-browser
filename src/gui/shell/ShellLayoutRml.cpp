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
    // The tab page's bars (nav bar, a settings page's bar) are the frosted ones while nothing covers them.
    if (frost == CompactChromeFrostSurface::BottomNav && frost_enabled) {
      out << " shell-nav-page--frost";
    }
    // Root pages (Chats / Contacts / Me): the nav bar's small title and fade follow the scroll.
    out << "\" id=\"shell-nav-page\" data-class-large-title-collapsed=\"large_title_collapsed\">";
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
    out << "<svg src=\"../icons/v2/back.svg\"></svg>";
    out << "</button>";
    out << "<div class=\"bar-fade\"></div>";
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
    if (state_.nav_tab == NavTab::Home) {
      // Home: the tab bar takes the ask-AI dock's colour, one bottom area with no line or seam between them.
      out << " shell-bottom-chrome--home";
    }
    out << "\" id=\"shell-bottom-chrome\">";
    out << "<div class=\"bar-fade bar-fade--up\"></div>";
    out << "<div class=\"shell-nav-rail shell-nav-rail--compact\" id=\"shell-nav-rail-mount\"></div>";
    out << "</div>";
  }

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
  // On phones the pushed page gets the compact class too, so the phone styles of the page it shows apply.
  const char* compact = state_.layout_mode == LayoutMode::Compact ? " shell-layer-compact" : "";
  out << "<div class=\"shell-layer shell-layer-transient" << compact
      << "\" id=\"shell-transient-layer\" data-model=\"window\">";
  // The contact page has a titled bar on phones, the colour of every phone bar, no lines. So does the blocked list.
  const bool blocked_list = compact[0] != '\0' && top.spec.key == "blocked_people";
  const bool titled = compact[0] != '\0' && (top.spec.key == "contact_detail" || blocked_list);
  out << "<div class=\"shell-transient-chrome " << (titled ? "shell-transient-chrome--titled " : "")
      << SurfaceChromeClass(CompactChromeFrostSurface::TransientHeader, frost, frost_enabled, solid) << "\">";
  out << "<button class=\"shell-back-btn\" type=\"button\" data-event-click=\"transient_back()\">"
         "<svg src=\"../icons/v2/back.svg\"></svg>"
         "</button>";
  if (blocked_list) {
    out << "<p class=\"shell-transient-title\">" << Tr("contacts.blocked.title") << "</p>";
  } else if (titled) {
    // The contacts model is the one the page below binds; member_preview is a group member who is not a contact.
    out << "<p class=\"shell-transient-title\" data-model=\"contacts\">"
           "<span data-if=\"selected.member_preview\">" << Tr("contacts.member_info") << "</span>"
           "<span data-if=\"!selected.member_preview\">" << Tr("contacts.info") << "</span></p>";
  }
  out << "<div class=\"bar-fade\"></div>";
  out << "</div>";
  out << PaneSlot(top.spec.key, "shell-pane-transient");
  out << "</div>";
  return out.str();
}

std::string ShellLayoutRml::Overlays() const {
  std::ostringstream out;
  for (const OverlayEntry& overlay : state_.overlay_stack) {
    if (overlay.full_screen && state_.layout_mode == LayoutMode::Compact) {
      // Phone full-screen page (the scanner): no scrim, frame or close button; the view has its own back. ShellHost
      // lifts the layer under the status bar and pads the body by the safe areas.
      out << "<div class=\"shell-layer shell-layer-overlay shell-layer-full-screen\" id=\"shell-full-screen-"
          << overlay.id << "\" data-model=\"window\">";
      out << "<div class=\"shell-overlay-body shell-layer-compact\" id=\"overlay-body-" << overlay.id << "\"></div>";
      out << "</div>";
      continue;
    }
    if (overlay.page_sheet && state_.layout_mode == LayoutMode::Compact) {
      // Phone page sheet: the scrim dims the page behind; ShellHost slides the sheet in and attaches its drag
      // (ShellPageSheetGesture). The view keeps its own Cancel and its title row (.sheet-header); the close button
      // sits at the right end of that row: a 44dp target around a small grey disc with the X.
      // The stop's class sets the top edge (ShellHost switches to full height while the keyboard is up); at half and
      // small the page behind is dimmed less.
      const SheetDetent detent = overlay.sheet_detent;
      const char* stop = detent == SheetDetent::Small   ? " shell-page-sheet--small"
                         : detent == SheetDetent::Half  ? " shell-page-sheet--half"
                         : detent == SheetDetent::Large ? " shell-page-sheet--large"
                                                        : "";
      const char* layer_stop = detent == SheetDetent::Small  ? " shell-layer-sheet--small"
                               : detent == SheetDetent::Half ? " shell-layer-sheet--half"
                                                             : "";
      out << "<div class=\"shell-layer shell-layer-overlay shell-layer-sheet" << layer_stop << "\" data-model=\"window\">";
      out << "<div class=\"shell-scrim\" data-event-click=\"close_layer(" << overlay.id << ")\"></div>";
      // The view's key as a modifier class: a page with its own edge-to-edge rows (New chat) drops the side padding.
      out << "<div class=\"shell-page-sheet shell-page-sheet--" << overlay.key << stop << "\" id=\"shell-page-sheet-"
          << overlay.id << "\">";
      out << "<div class=\"shell-page-sheet-grabber\"></div>";
      out << "<button class=\"shell-close-btn shell-page-sheet-close\" type=\"button\" data-event-click=\"close_layer("
          << overlay.id << ")\">";
      out << "<span class=\"sheet-icon-disc\"><svg src=\"../icons/v2/close.svg\" width=\"12\" height=\"12\"></svg></span>";
      out << "</button>";
      // The compact class: a phone page shown in a sheet (New chat) keeps its phone styles.
      out << "<div class=\"shell-overlay-body shell-layer-compact\" id=\"overlay-body-" << overlay.id << "\"></div>";
      out << "</div></div>";
      continue;
    }
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

bool ShellLayoutRml::DialogButtonsStacked(const std::string& cancel_label, const std::string& ok_label) {
  // Two buttons share the 270dp card side by side (17dp text, a few dp of padding each); a label that would
  // not fit in half of it sends the pair to a column, like UIAlertController does.
  constexpr float kCjkDp = 17.f;
  constexpr float kLatinDp = 9.f;
  constexpr float kHalfButtonTextDp = 112.f;
  const auto text_width = [](const std::string& label) {
    float width = 0.f;
    for (const unsigned char c : label) {
      if (c >= 0xE0) {
        width += kCjkDp; // lead byte of a 3-byte sequence (CJK)
      } else if (c < 0x80) {
        width += kLatinDp;
      }
    }
    return width;
  };
  return text_width(cancel_label) > kHalfButtonTextDp || text_width(ok_label) > kHalfButtonTextDp;
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
  const std::string ok_label =
      state_.dialog.ok_label.empty() ? Tr("common.ok") : std::string(state_.dialog.ok_label.c_str());
  const std::string cancel_label = Tr("common.cancel");
  const bool stacked = state_.dialog.show_cancel && DialogButtonsStacked(cancel_label, ok_label);
  out << "<div class=\"shell-dialog shell-dialog--alert" << (state_.dialog.destructive ? " shell-dialog--destructive" : "")
      << "\">";
  out << "<div class=\"shell-dialog-body\">";
  out << "<h2 class=\"heading-2 shell-dialog-title\">{{dialog_title}}</h2>";
  out << "<p class=\"text shell-dialog-message\">{{dialog_message}}</p>";
  out << "<label class=\"shell-dialog-checkbox row\" data-if=\"dialog_show_checkbox\">";
  out << "<input type=\"checkbox\" data-value=\"dialog_checkbox_checked\" "
         "data-event-change=\"dialog_toggle_checkbox()\"/>";
  out << "<span>{{dialog_checkbox_label}}</span>";
  out << "</label>";
  out << "<input class=\"shell-dialog-prompt\" type=\"text\" data-if=\"dialog_show_prompt\" "
         "data-value=\"dialog_prompt_value\"/>";
  out << "</div>";
  out << "<div class=\"shell-dialog-actions row" << (stacked ? " shell-dialog-actions--stack" : "") << "\">";
  out << "<button class=\"btn btn-secondary shell-dialog-cancel\" data-if=\"dialog_show_cancel\" "
         "data-event-click=\"dialog_cancel()\">"
      << cancel_label << "</button>";
  out << "<button class=\"btn btn-primary shell-dialog-ok" << (state_.dialog.show_cancel ? " shell-dialog-ok--split" : "")
      << "\" data-event-click=\"dialog_ok()\">" << ok_label << "</button>";
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
  out << "<button class=\"btn btn-primary\" data-event-click=\"pin_gate_identity_new()\">"
      << Tr("pin.identity_new") << "</button>";
  out << "<button class=\"btn btn-secondary\" data-event-click=\"pin_gate_identity_link()\">"
      << Tr("pin.identity_link") << "</button>";
  out << "<button class=\"btn btn-ghost\" data-event-click=\"pin_gate_identity_phrase()\">"
      << Tr("pin.identity_phrase") << "</button>";
  out << "</div>";
  out << "<div class=\"shell-pin-gate-chooser\" data-if=\"pin_gate_chooser_mode\">";
  out << "<button class=\"btn btn-primary\" data-event-click=\"pin_gate_set_pin()\">" << Tr("pin.set_pin")
      << "</button>";
  out << "<button class=\"btn btn-secondary\" data-event-click=\"pin_gate_use_default()\">"
      << Tr("pin.just_continue") << "</button>";
  out << "<button class=\"btn btn-ghost\" data-event-click=\"pin_gate_cancel()\">" << Tr("common.back")
      << "</button>";
  out << "</div>";
  out << "<textarea class=\"field shell-pin-gate-input\" data-if=\"pin_gate_link_paste_mode && !pin_gate_phrase_flow\" "
         "data-value=\"pin_gate_link_payload\" maxlength=\"65536\" placeholder=\""
      << Tr("pin.link_paste_placeholder") << "\"></textarea>";
  out << "<textarea class=\"field shell-pin-gate-input\" data-if=\"pin_gate_link_paste_mode && pin_gate_phrase_flow\" "
         "data-value=\"pin_gate_link_payload\" maxlength=\"65536\" placeholder=\""
      << Tr("pin.phrase_paste_placeholder") << "\"></textarea>";
  out << "<p class=\"text muted shell-pin-gate-note\" "
         "data-if=\"pin_gate_link_paste_mode && pin_gate_phrase_flow && pin_gate_saved_copy_name != ''\">"
      << Tr("pin.phrase_saved_copy") << " {{pin_gate_saved_copy_name}}</p>";
  out << "<button class=\"btn btn-ghost\" data-if=\"pin_gate_link_paste_mode && pin_gate_phrase_flow\" "
         "data-event-click=\"pin_gate_use_saved_copy()\">"
      << Tr("pin.phrase_use_saved_copy") << "</button>";
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
  out << "<button class=\"btn btn-secondary\" data-if=\"pin_gate_link_paste_mode\" "
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
  // The dialog lives only in its mount below: SyncLayout fills the mount right after this root is
  // built, so an inline copy here would leave a second dialog that dialog_close never removes
  // (scan -> confirm: cancel left a dead OK button and a scrim over the whole app, 2026-10-28).
  // Overlay chrome mounts stay empty here; Remount* fills them (avoids full-shell remount).
  out << "<div id=\"shell-dialog-mount\" class=\"shell-overlay-chrome-mount shell-overlay-chrome-mount--dialog\"></div>";
  out << "<div id=\"shell-photo-mount\" class=\"shell-overlay-chrome-mount shell-overlay-chrome-mount--photo\"></div>";
  out << "<div id=\"shell-call-in-progress-mount\" class=\"shell-call-chrome-mount shell-call-chrome-mount--bar\"></div>";
  out << "<div id=\"shell-call-ring-mount\" class=\"shell-call-chrome-mount shell-call-chrome-mount--ring\"></div>";
  out << "<div id=\"shell-pin-gate-mount\" class=\"shell-overlay-chrome-mount shell-overlay-chrome-mount--pin\"></div>";
  out << "<div id=\"shell-onboarding-mount\" class=\"shell-overlay-chrome-mount shell-overlay-chrome-mount--pin\"></div>";
  return out.str();
}

} // namespace pbr
