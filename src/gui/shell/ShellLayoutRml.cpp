#include "gui/shell/ShellLayoutRml.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/ui/ShellInterruption.h"
#include "lib/ui/ShellLayout.h"

#include <sstream>
#include <utility>

namespace pbr {

namespace {

/** A bar's material: a child element carrying the bar colour and, when this bar is a frosted surface, the backdrop
 *  blur. The engine draws no backdrop blur on a bar that has a z-index and children itself, so the blur goes on this
 *  child (like .pane-nav-bg and .nav-bar-bg). Put it first in the bar; its content is positioned above it. */
std::string BarMaterial(bool frosted, bool frost_enabled, bool solid) {
  std::string out = "<div class=\"bar-material";
  if (frosted && frost_enabled && !solid) {
    out += " bar-material--frost";
  }
  return out + "\"></div>";
}

std::string SurfaceChromeClass(CompactChromeFrostSurface surface, CompactChromeFrostSurface active, bool frost_enabled,
                               bool solid) {
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
  const CompactChromeFrost frosts = ShellInterruption::ResolveFrost(state_);
  const CompactChromeFrostSurface frost = frosts.top;
  const bool frost_enabled = ChromeFrostEnabled();
  const bool solid = state_.reduce_transparency;
  std::ostringstream out;
  out << "<div class=\"shell-layer shell-layer-base shell-layer-compact\" data-model=\"window\">";

  // A hidden tab's page (Me, Contacts: DEV/PLAN-minimal-nav.md A) is drawn instead of a chat overlay left open under
  // it; the overlay is drawn again on the way back.
  const bool hidden_tab = ShellLayout::HiddenTabShown(state_);
  const bool chat_shown = state_.compact_chat_open && !hidden_tab;
  if (!chat_shown) {
    const bool home_inline = state_.nav_tab == NavTab::Home;
    out << "<div class=\"shell-nav-page";
    if (home_inline) {
      out << " shell-nav-page--home";
    }
    if (hidden_tab) {
      // Its bar: back, the title always shown, no large title, and no tab bar under it (Settings keeps the tab bar:
      // its content then clears it, TODO #200).
      out << " shell-nav-page--hidden-tab";
      if (ShellLayout::SettingsTabBarShown(state_)) {
        out << " shell-nav-page--tab-bar";
      }
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

  if (chat_shown) {
    out << "<div class=\"shell-chat-overlay\" id=\"shell-chat-overlay\">";
    out << "<div class=\"shell-chat-overlay-chrome row "
        << SurfaceChromeClass(CompactChromeFrostSurface::ChatOverlayHeader, frost, frost_enabled, solid) << "\">";
    out << BarMaterial(frost == CompactChromeFrostSurface::ChatOverlayHeader, frost_enabled, solid);
    out << "<button class=\"shell-back-btn shell-circle-btn\" type=\"button\" "
           "data-event-click=\"compact_chat_back()\">";
    out << "<span class=\"shell-circle-btn-face\"><svg src=\"../icons/v2/back.svg\"></svg></span>";
    out << "</button>";
    // An AI conversation's bar: a new conversation on the right (DEV/PLAN-minimal-nav.md B). thread_is_ai is the chat
    // model's, new_chat the shell model's (ChatController).
    out << "<div class=\"shell-chat-new-slot\" data-model=\"chat\" data-if=\"thread_is_ai\">";
    out << "<button class=\"shell-circle-btn shell-chat-new-btn\" type=\"button\" data-model=\"shell\" "
           "data-event-click=\"new_chat()\">";
    out << "<span class=\"shell-circle-btn-face\"><svg src=\"../icons/v2/compose.svg\"></svg></span>";
    out << "</button></div>";
    out << "<div class=\"bar-fade\"></div>";
    out << "</div>";
    out << "<div class=\"shell-pane-body\" id=\"pane-body-chat\"></div>";
    // The composer bar is clear (TODO #202: a floating pill and two round buttons, the transcript scrolls under them);
    // its height is the mount's, whose content is replaced on every mount.
    out << "<div class=\"shell-composer-bar\">";
    out << "<div class=\"shell-composer-mount surface-chrome" << (solid ? " surface-chrome--solid" : "")
        << "\" id=\"shell-composer-mount\"></div>";
    out << "</div>";
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

  // The tab bar only where it is drawn (ShellLayout::CompactTabBarDp): not on a hidden tab or under the chat.
  if (ShellLayout::CompactTabBarDp(state_) > 0.f) {
    out << "<div class=\"shell-bottom-chrome";
    if (frosts.bottom == CompactChromeFrostSurface::BottomNav && frost_enabled) {
      out << " shell-bottom-chrome--frost";
    }
    if (!state_.transient_stack.empty()) {
      // Settings' pushed pages (contact, blocked list) are drawn under the bar, not over it.
      out << " shell-bottom-chrome--over-page";
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
    out << "<div class=\"shell-nav-rail shell-nav-rail--compact shell-nav-rail--text\" "
           "id=\"shell-nav-rail-mount\"></div>";
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
  // New chat and Add contact (TODO #176 / #177) are titled the same way: their title row is the page's, the list scrolls
  // under the frosted bar. The contact page and the blocked list have a 48dp bar on phones with only the back arrow, the colour of every phone
  // bar, no lines; it hides while the page is scrolled away from its top (ShellNavBarScroll).
  const bool titled = compact[0] != '\0' && (top.spec.key == "contact_detail" || top.spec.key == "blocked_people" ||
                                             top.spec.key == "new_chat" || top.spec.key == "add_contact");
  out << "<div class=\"shell-layer shell-layer-transient" << compact << (titled ? " nav-bar-host" : "")
      << "\" id=\"shell-transient-layer\" data-model=\"window\">";
  out << "<div class=\"shell-transient-chrome " << (titled ? "shell-transient-chrome--titled " : "")
      << SurfaceChromeClass(CompactChromeFrostSurface::TransientHeader, frost, frost_enabled, solid) << "\">";
  if (titled) {
    out << "<div class=\"nav-bar-bg\"></div>";
  }
  if (compact[0] != '\0') {
    // Phones: the round back (DEV/PLAN-minimal-nav.md G); shell-back-btn keeps it chrome for the swipe-back.
    out << "<button class=\"shell-back-btn shell-circle-btn\" type=\"button\" data-event-click=\"transient_back()\">"
           "<span class=\"shell-circle-btn-face\"><svg src=\"../icons/v2/back.svg\"></svg></span>"
           "</button>";
  } else {
    out << "<button class=\"shell-back-btn\" type=\"button\" data-event-click=\"transient_back()\">"
           "<svg src=\"../icons/v2/back.svg\"></svg>"
           "</button>";
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
      out << "<div class=\"shell-layer shell-layer-overlay shell-layer-sheet" << layer_stop
          << "\" data-model=\"window\">";
      out << "<div class=\"shell-scrim\" data-event-click=\"close_layer(" << overlay.id << ")\"></div>";
      // The view's key as a modifier class: a page with its own edge-to-edge rows (New chat) drops the side padding.
      // Frosted bars inside it (the ask-AI panel's input bar) follow the Transparency setting like the top bars.
      out << "<div class=\"shell-page-sheet shell-page-sheet--" << overlay.key << stop
          << (ChromeFrostEnabled() ? " shell-page-sheet--frost" : "") << "\" id=\"shell-page-sheet-" << overlay.id
          << "\">";
      out << "<div class=\"shell-page-sheet-grabber\"></div>";
      out << "<button class=\"shell-close-btn shell-page-sheet-close\" type=\"button\" data-event-click=\"close_layer("
          << overlay.id << ")\">";
      out << "<span class=\"sheet-icon-disc\"><svg src=\"../icons/v2/close.svg\" width=\"12\" "
             "height=\"12\"></svg></span>";
      out << "</button>";
      // The compact class: a phone page shown in a sheet (New chat) keeps its phone styles.
      out << "<div class=\"shell-overlay-body shell-layer-compact\" id=\"overlay-body-" << overlay.id << "\"></div>";
      out << "</div></div>";
      continue;
    }
    out << "<div class=\"shell-layer shell-layer-overlay\" data-model=\"window\">";
    out << "<div class=\"shell-scrim\" data-event-click=\"close_layer(" << overlay.id << ")\"></div>";
    out << "<div class=\"shell-frame\">";
    out << "<button class=\"shell-close-btn\" type=\"button\" data-event-click=\"close_layer(" << overlay.id << ")\">";
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
  // The rounded sheet stands on the panel, which is the composer's colour, so the bar above runs on behind its corners.
  out << "<div class=\"shell-emoji-keyboard-sheet\">";
  out << "<div class=\"shell-emoji-keyboard-body\" id=\"shell-bottom-chrome-body\"></div>";
  out << "</div></div></div>";
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
  out << "<div class=\"shell-dialog shell-dialog--alert"
      << (state_.dialog.destructive ? " shell-dialog--destructive" : "") << "\">";
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
  out << "<button class=\"btn btn-primary shell-dialog-ok"
      << (state_.dialog.show_cancel ? " shell-dialog-ok--split" : "") << "\" data-event-click=\"dialog_ok()\">"
      << ok_label << "</button>";
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
  out << "<p class=\"text muted shell-pin-gate-note\" data-if=\"pin_gate_biometric_hint != ''\">"
         "{{pin_gate_biometric_hint}}</p>";
  out << "<div class=\"shell-pin-gate-chooser\" data-if=\"pin_gate_identity_fork_mode\">";
  out << "<button class=\"btn btn-primary\" data-event-click=\"pin_gate_identity_new()\">" << Tr("pin.identity_new")
      << "</button>";
  out << "<button class=\"btn btn-secondary\" data-event-click=\"pin_gate_identity_link()\">" << Tr("pin.identity_link")
      << "</button>";
  out << "<button class=\"btn btn-ghost\" data-event-click=\"pin_gate_identity_phrase()\">" << Tr("pin.identity_phrase")
      << "</button>";
  out << "</div>";
  out << "<div class=\"shell-pin-gate-chooser\" data-if=\"pin_gate_chooser_mode\">";
  out << "<button class=\"btn btn-primary\" data-event-click=\"pin_gate_set_pin()\">" << Tr("pin.set_pin")
      << "</button>";
  out << "<button class=\"btn btn-secondary\" data-event-click=\"pin_gate_use_default()\">" << Tr("pin.just_continue")
      << "</button>";
  out << "<button class=\"btn btn-ghost\" data-event-click=\"pin_gate_cancel()\">" << Tr("common.back") << "</button>";
  out << "</div>";
  out << "<textarea class=\"field shell-pin-gate-input\" data-if=\"pin_gate_link_paste_mode && !pin_gate_phrase_flow "
         "&& !pin_gate_payload_hidden\" "
         "data-value=\"pin_gate_link_payload\" maxlength=\"65536\" placeholder=\""
      << Tr("pin.link_paste_placeholder") << "\"></textarea>";
  out << "<textarea class=\"field shell-pin-gate-input\" data-if=\"pin_gate_link_paste_mode && pin_gate_phrase_flow && "
         "!pin_gate_payload_hidden\" "
         "data-value=\"pin_gate_link_payload\" maxlength=\"65536\" placeholder=\""
      << Tr("pin.phrase_paste_placeholder") << "\"></textarea>";
  out << "<button class=\"btn btn-secondary shell-pin-gate-paste\" data-if=\"pin_gate_link_paste_mode && "
         "!pin_gate_payload_hidden\" "
         "data-event-click=\"pin_gate_paste()\">"
      << Tr("pin.paste_button") << "</button>";
  // B166/B167: a transfer code is a multi-KB blob; rendering it in the textarea froze the app. Once a paste is too big
  // to render we keep it in the model (submit uses it) but do NOT show it -- a short confirmation + Clear instead.
  out << "<p class=\"text shell-pin-gate-note\" "
         "data-if=\"pin_gate_link_paste_mode && pin_gate_payload_hidden\">"
      << Tr("pin.link_pasted") << "</p>";
  out << "<button class=\"btn btn-ghost\" "
         "data-if=\"pin_gate_link_paste_mode && pin_gate_payload_hidden\" "
         "data-event-click=\"pin_gate_clear_payload()\">"
      << Tr("pin.clear_pasted") << "</button>";
  out << "<p class=\"text muted shell-pin-gate-note\" "
         "data-if=\"pin_gate_link_paste_mode && pin_gate_phrase_flow && !pin_gate_payload_hidden && "
         "pin_gate_saved_copy_name != ''\">"
      << Tr("pin.phrase_saved_copy") << " {{pin_gate_saved_copy_name}}</p>";
  out << "<button class=\"btn btn-ghost\" data-if=\"pin_gate_link_paste_mode && pin_gate_phrase_flow && "
         "!pin_gate_payload_hidden\" "
         "data-event-click=\"pin_gate_use_saved_copy()\">"
      << Tr("pin.phrase_use_saved_copy") << "</button>";
  if (state_.pin_gate.biometric_first) {
    PinGateBiometricFirst(out);
  } else if (state_.pin_gate.keypad) {
    PinGateKeypad(out);
  } else {
    PinGateFields(out);
  }
  // PIN grace window: a call waits for the locked phone's unlock; the PIN stays one tap away.
  out << "<button class=\"btn btn-secondary\" data-if=\"pin_gate_waiting_device_unlock\" "
         "data-event-click=\"pin_gate_use_pin_instead()\">"
      << Tr("pin.unlock_use_pin_instead") << "</button>";
  out << "<button class=\"btn btn-ghost\" data-if=\"pin_gate_unlock_mode && !pin_gate_waiting_device_unlock\" "
         "data-event-click=\"pin_gate_forgot_toggle()\">"
      << Tr("pin.forgot_link") << "</button>";
  out << "<p class=\"text muted shell-pin-gate-note\" data-if=\"pin_gate_unlock_mode && pin_gate_forgot_open\">"
      << Tr("pin.forgot_explain") << "</p>";
  out << "</div></div>";
  return out.str();
}

bool ShellLayoutRml::PinGateFocusesField(const PinGateState& gate) {
  return gate.active && !gate.keypad && !gate.biometric_first && !gate.chooser_mode && !gate.identity_fork_mode &&
         !gate.link_paste_mode && !gate.waiting_device_unlock;
}

// The field mode: the PIN (and a new PIN's confirmation) in password fields with the system keyboard; Continue submits.
void ShellLayoutRml::PinGateFields(std::ostringstream& out) const {
  out << "<input id=\"" << kPinGateFieldId
      << "\" class=\"field shell-pin-gate-input\" type=\"password\" "
         "data-if=\"!pin_gate_chooser_mode && !pin_gate_identity_fork_mode && !pin_gate_link_paste_mode && "
         "!pin_gate_waiting_device_unlock\" "
         "data-value=\"pin_gate_pin\" placeholder=\""
      << Tr("pin.placeholder") << "\"/>";
  out << "<input class=\"field shell-pin-gate-input\" type=\"password\" "
         "data-if=\"pin_gate_ask_confirm && !pin_gate_chooser_mode\" data-value=\"pin_gate_pin_confirm\" "
         "placeholder=\""
      << Tr("pin.confirm_placeholder") << "\"/>";
  out << "<div class=\"shell-dialog-actions row\" "
         "data-if=\"!pin_gate_chooser_mode && !pin_gate_identity_fork_mode && !pin_gate_waiting_device_unlock\">";
  out << "<button class=\"btn btn-secondary\" data-if=\"pin_gate_create_mode\" "
         "data-event-click=\"pin_gate_cancel()\">"
      << Tr("pin.not_now") << "</button>";
  out << "<button class=\"btn btn-secondary\" data-if=\"pin_gate_verify_mode || pin_gate_change_mode\" "
         "data-event-click=\"pin_gate_cancel()\">"
      << Tr("common.cancel") << "</button>";
  out << "<button class=\"btn btn-secondary\" data-if=\"pin_gate_link_paste_mode\" "
         "data-event-click=\"pin_gate_cancel()\">"
      << Tr("common.back") << "</button>";
  out << "<button class=\"btn btn-primary\" data-event-click=\"pin_gate_submit()\">" << Tr("common.continue")
      << "</button>";
  out << "</div>";
  // Back to the number pad: a PIN known to be digits, or a new PIN on a phone.
  out << "<button class=\"btn btn-ghost shell-pin-gate-link\" "
         "data-if=\"pin_gate_keypad_offered && !pin_gate_link_paste_mode && !pin_gate_waiting_device_unlock\" "
         "data-event-click=\"pin_gate_use_keypad()\">"
      << Tr("pin.keypad_use_digits") << "</button>";
  // Face / fingerprint unlock on: a text PIN (or a pad switched to the field) can prompt again too.
  for (const char* kind : {"face", "touch", "fingerprint"}) {
    out << "<button class=\"btn btn-ghost shell-pin-gate-link\" data-if=\"pin_gate_biometric_key && "
           "pin_gate_biometric_kind == '"
        << kind << "' && !pin_gate_waiting_device_unlock\" data-event-click=\"pin_gate_biometric()\">"
        << Tr(std::string("pin.biometric_link.") + kind) << "</button>";
  }
}

// Face / fingerprint unlock on (at-rest A014): the door opens like the phone's own lock screen -- the sensor's key
// (prompts again) and "Enter PIN"; the pad / field come only after that button.
void ShellLayoutRml::PinGateBiometricFirst(std::ostringstream& out) const {
  out << "<div class=\"shell-pin-bio\" data-if=\"!pin_gate_waiting_device_unlock\">";
  out << "<button class=\"shell-pin-bio-key\" data-event-click=\"pin_gate_biometric()\">"
         "<svg class=\"shell-pin-bio-icon\" data-if=\"pin_gate_biometric_kind == 'face'\" "
         "src=\"../icons/v2/face-id.svg\" width=\"64\" height=\"64\"></svg>"
         "<svg class=\"shell-pin-bio-icon\" data-if=\"pin_gate_biometric_kind != 'face'\" "
         "src=\"../icons/v2/fingerprint.svg\" width=\"64\" height=\"64\"></svg>"
         "</button>";
  out << "<button class=\"btn btn-secondary shell-pin-bio-pin\" data-event-click=\"pin_gate_use_pin()\">"
      << Tr("pin.biometric_use_pin") << "</button>";
  out << "</div>";
}

// The number pad (phones, a digit PIN; DEV/PLAN-pin-gate-redesign.md B): a row of dots, 3 x 4 keys, no input element
// (no system keyboard). A full PIN submits itself; a new PIN's first entry goes on with "OK" (bottom left).
void ShellLayoutRml::PinGateKeypad(std::ostringstream& out) const {
  out << "<div class=\"shell-pin-pad\" data-if=\"!pin_gate_waiting_device_unlock\">";
  out << "<div class=\"shell-pin-dots\" data-class-shell-pin-dots--busy=\"pin_gate_keypad_busy\">";
  for (int i = 0; i < 8; ++i) {
    out << "<span class=\"shell-pin-dot\" data-if=\"pin_gate_keypad_slots > " << i
        << "\" data-class-shell-pin-dot--filled=\"pin_gate_keypad_filled > " << i << "\"></span>";
  }
  out << "</div>";
  out << "<div class=\"shell-pin-keys\">";
  for (int digit = 1; digit <= 9; ++digit) {
    out << "<button class=\"shell-pin-key\" data-event-click=\"pin_gate_key(" << digit << ")\">" << digit
        << "</button>";
  }
  out << "<button class=\"shell-pin-key shell-pin-key--plain shell-pin-key--word\" "
         "data-if=\"pin_gate_keypad_ok_shown\" "
         "data-event-click=\"pin_gate_ok()\">"
      << Tr("pin.keypad_ok") << "</button>";
  // Face / fingerprint unlock on (unlock only, where no "OK" is due): its key prompts again.
  out << "<button class=\"shell-pin-key shell-pin-key--plain\" "
         "data-if=\"pin_gate_biometric_key && !pin_gate_keypad_ok_shown\" data-event-click=\"pin_gate_biometric()\">"
         "<svg class=\"shell-pin-key-icon\" data-if=\"pin_gate_biometric_kind == 'face'\" "
         "src=\"../icons/v2/face-id.svg\" width=\"28\" height=\"28\"></svg>"
         "<svg class=\"shell-pin-key-icon\" data-if=\"pin_gate_biometric_kind != 'face'\" "
         "src=\"../icons/v2/fingerprint.svg\" width=\"28\" height=\"28\"></svg>"
         "</button>";
  out << "<span class=\"shell-pin-key shell-pin-key--blank\" "
         "data-if=\"!pin_gate_keypad_ok_shown && !pin_gate_biometric_key\"></span>";
  out << "<button class=\"shell-pin-key\" data-event-click=\"pin_gate_key(0)\">0</button>";
  out << "<button class=\"shell-pin-key shell-pin-key--plain\" data-event-click=\"pin_gate_backspace()\">"
         "<svg class=\"shell-pin-key-icon\" src=\"../icons/v2/delete-left.svg\" width=\"28\" height=\"28\"></svg>"
         "</button>";
  out << "</div>";
  // Always there: any PIN can be typed in the field (a kind remembered wrongly still gets in).
  out << "<button class=\"btn btn-ghost shell-pin-gate-link\" data-event-click=\"pin_gate_use_text()\">"
         "{{pin_gate_keypad_text_label}}</button>";
  out << "<div class=\"shell-dialog-actions row\" data-if=\"pin_gate_create_mode || pin_gate_verify_mode || "
         "pin_gate_change_mode\">";
  out << "<button class=\"btn btn-secondary\" data-if=\"pin_gate_create_mode\" data-event-click=\"pin_gate_cancel()\">"
      << Tr("pin.not_now") << "</button>";
  out << "<button class=\"btn btn-secondary\" data-if=\"pin_gate_verify_mode || pin_gate_change_mode\" "
         "data-event-click=\"pin_gate_cancel()\">"
      << Tr("common.cancel") << "</button>";
  out << "</div>";
  out << "</div>";
}

std::string ShellLayoutRml::AiDrawer() const {
  if (state_.layout_mode != LayoutMode::Compact || !state_.ai_drawer_open) {
    return {};
  }
  std::ostringstream out;
  // The compact class: the phone styles of the rows and the round buttons in the view apply.
  out << "<div class=\"shell-layer shell-layer-compact shell-drawer-layer\" data-model=\"window\">";
  out << "<div class=\"shell-scrim shell-drawer-scrim\" data-event-click=\"close_ai_drawer()\"></div>";
  // The edge shadow rides with the drawer; the view goes into the body (ShellHost::MountAiDrawer).
  // Its top bar and "you" row are frosted like the top bars when Transparency is on (colors.rcss.tmpl).
  out << "<div class=\"shell-drawer" << (ChromeFrostEnabled() ? " shell-drawer--frost" : "")
      << "\"><div class=\"shell-drawer-edge\"></div>";
  out << "<div class=\"shell-drawer-body\" id=\"shell-ai-drawer-mount\"></div></div>";
  out << "</div>";
  return out.str();
}

std::string ShellLayoutRml::ShellRoot() const {
  std::ostringstream out;
  if (state_.layout_mode == LayoutMode::Expanded) {
    out << ExpandedBase();
  } else {
    out << CompactBase();
  }
  // Filled by ShellHost::RemountAiDrawerNow (AiDrawer), so opening / closing the drawer never rebuilds this root.
  out << "<div id=\"shell-ai-drawer-host\" class=\"shell-drawer-host\"></div>";
  out << TransientLayer();
  out << Overlays();
  // The dialog lives only in its mount below: SyncLayout fills the mount right after this root is
  // built, so an inline copy here would leave a second dialog that dialog_close never removes
  // (scan -> confirm: cancel left a dead OK button and a scrim over the whole app, 2026-10-28).
  // Overlay chrome mounts stay empty here; Remount* fills them (avoids full-shell remount).
  out << "<div id=\"shell-dialog-mount\" class=\"shell-overlay-chrome-mount "
         "shell-overlay-chrome-mount--dialog\"></div>";
  out << "<div id=\"shell-photo-mount\" class=\"shell-overlay-chrome-mount shell-overlay-chrome-mount--photo\"></div>";
  out << "<div id=\"shell-call-in-progress-mount\" class=\"shell-call-chrome-mount "
         "shell-call-chrome-mount--bar\"></div>";
  out << "<div id=\"shell-call-ring-mount\" class=\"shell-call-chrome-mount shell-call-chrome-mount--ring\"></div>";
  out << "<div id=\"shell-pin-gate-mount\" class=\"shell-overlay-chrome-mount shell-overlay-chrome-mount--pin\"></div>";
  out << "<div id=\"shell-onboarding-mount\" class=\"shell-overlay-chrome-mount "
         "shell-overlay-chrome-mount--pin\"></div>";
  return out.str();
}

} // namespace pbr
