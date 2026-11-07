#include "gui/shell/ShellLayoutRml.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

bool Has(const std::string& rml, const std::string& part) {
  return rml.find(part) != std::string::npos;
}

ShellState WithChatPane(const LayoutMode mode) {
  ShellState state;
  state.layout_mode = mode;
  state.nav_tab = NavTab::Sessions;
  state.panes.push_back({.spec = {.key = "chat", .rml_path = "views/chat.rml", .provides_composer = true}, .id = 1});
  state.primary_pane_key = "chat";
  return state;
}

TEST(ShellLayoutRmlTest, AnExpandedWindowHasTheRailTheListAndThePrimaryPane) {
  const std::string rml = ShellLayoutRml(WithChatPane(LayoutMode::Expanded), false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "shell-nav-rail-mount"));
  EXPECT_TRUE(Has(rml, "shell-nav-content-mount")) << "Sessions has a list beside the chat";
  EXPECT_TRUE(Has(rml, "id=\"pane-chat\""));
  EXPECT_TRUE(Has(rml, "id=\"pane-composer-chat\"")) << "the chat pane carries the composer";
}

TEST(ShellLayoutRmlTest, ACompactWindowHasTheTabBarAndMeIsAPlainTab) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.compact_chat_open = false;
  state.nav_tab = NavTab::Me;
  const std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "id=\"shell-bottom-chrome\""));
  EXPECT_TRUE(Has(rml, "id=\"shell-nav-rail-mount\"")) << "the tab bar mounts here";
  EXPECT_TRUE(Has(rml, "id=\"pane-body-settings\"")) << "Me fills the page like any other tab";
  EXPECT_FALSE(Has(rml, "account-sheet"));
  EXPECT_FALSE(Has(rml, "close_account_sheet"));
}

// Kenneth 2026-11-03: on Home the dock and the tab bar read as two hard lines; there they are one bottom area.
TEST(ShellLayoutRmlTest, ThePhoneTabIconsAre28dpAndTheLabels11dp) {
  // Kenneth 2026-11-04 (PLAN-ui-refresh-impl 62.6): the icons one step bigger, the words as they were.
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "themes/components.rcss");
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string css = buffer.str();
  const size_t icon = css.find(".shell-nav-rail--compact .shell-nav-tab svg {");
  ASSERT_NE(icon, std::string::npos);
  EXPECT_EQ(css.find("width: 28dp;\n  height: 28dp;", icon), css.find("width:", icon));
  const size_t label = css.find(".shell-nav-rail--compact .shell-nav-tab-label {");
  ASSERT_NE(label, std::string::npos);
  EXPECT_EQ(css.find("font-size: 11dp;", label), css.find("font-size:", label));
}

TEST(ShellLayoutRmlTest, OnlyHomeMarksTheTabBarToBlendWithTheDock) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.compact_chat_open = false;
  state.nav_tab = NavTab::Home;
  EXPECT_TRUE(Has(ShellLayoutRml(state, false, "").ShellRoot(), "shell-bottom-chrome--home"));
  for (const NavTab tab : {NavTab::Sessions, NavTab::Contacts, NavTab::Me}) {
    state.nav_tab = tab;
    EXPECT_FALSE(Has(ShellLayoutRml(state, false, "").ShellRoot(), "shell-bottom-chrome--home"));
  }
}

// Every phone sheet (Kenneth 2026-11-04): its stop is its class (4/5 = --large, --half and --small also lighten the
// scrim, full has none), and its X is a 44dp button around a small grey disc.
TEST(ShellLayoutRmlTest, ASheetCarriesTheClassOfItsStop) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  OverlayEntry sheet;
  sheet.id = 7;
  sheet.key = "new_chat";
  sheet.page_sheet = true;
  state.overlay_stack.push_back(sheet);
  std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "shell-page-sheet--large")) << "every sheet opens at 4/5";
  EXPECT_FALSE(Has(rml, "--half"));
  EXPECT_TRUE(Has(rml, "<span class=\"sheet-icon-disc\"><svg src=\"../icons/v2/close.svg\" width=\"12\""));
  state.overlay_stack.back().sheet_detent = SheetDetent::Half;
  rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "shell-page-sheet--half"));
  EXPECT_TRUE(Has(rml, "shell-layer-sheet--half"));
  EXPECT_FALSE(Has(rml, "shell-page-sheet--large"));
  state.overlay_stack.back().sheet_detent = SheetDetent::Small;
  rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "shell-page-sheet--small"));
  EXPECT_TRUE(Has(rml, "shell-layer-sheet--small")) << "the scrim lightest at a third";
  EXPECT_FALSE(Has(rml, "--half"));
  state.overlay_stack.back().sheet_detent = SheetDetent::Full;
  rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_FALSE(Has(rml, "--half"));
  EXPECT_FALSE(Has(rml, "--small"));
  EXPECT_FALSE(Has(rml, "--large"));
}

// The phone scanner (Kenneth 2026-11-04, WeChat style): a full-screen layer with no scrim, frame or X of the shell's;
// the view brings its own back. Desktop keeps the centred panel with its close button.
TEST(ShellLayoutRmlTest, AFullScreenLayerHasNoScrimFrameOrCloseButtonOnPhones) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  OverlayEntry scan;
  scan.id = 9;
  scan.key = "qr_scan";
  scan.full_screen = true;
  state.overlay_stack.push_back(scan);
  std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "class=\"shell-layer shell-layer-overlay shell-layer-full-screen\" id=\"shell-full-screen-9\""));
  EXPECT_TRUE(Has(rml, "<div class=\"shell-overlay-body shell-layer-compact\" id=\"overlay-body-9\">"));
  EXPECT_FALSE(Has(rml, "close_layer(9)")) << "no scrim tap or X closes it: the view's back does";
  EXPECT_FALSE(Has(rml, "shell-frame"));
  state.layout_mode = LayoutMode::Expanded;
  rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_FALSE(Has(rml, "shell-layer-full-screen"));
  EXPECT_TRUE(Has(rml, "shell-frame"));
  EXPECT_TRUE(Has(rml, "close_layer(9)"));
}

TEST(ShellLayoutRmlTest, ACompactChatOverlayHasOneHeaderRowHoldingTheBackButton) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.compact_chat_open = true;
  const std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  const size_t chrome = rml.find("shell-chat-overlay-chrome");
  ASSERT_NE(chrome, std::string::npos);
  EXPECT_EQ(rml.find("shell-chat-overlay-chrome", chrome + 1), std::string::npos) << "exactly one bar";
  const size_t back = rml.find("compact_chat_back()", chrome);
  const size_t body = rml.find("id=\"pane-body-chat\"");
  ASSERT_NE(back, std::string::npos);
  ASSERT_NE(body, std::string::npos);
  EXPECT_LT(back, body) << "the back button is in the bar above the chat pane";
  EXPECT_TRUE(Has(rml, "icons/v2/back.svg"));
  // The title, avatar and menu are the chat pane's header row, lifted onto this bar by the theme; the shell
  // root must not draw a second one.
  EXPECT_FALSE(Has(rml, "chat-header"));
}

TEST(ShellLayoutRmlTest, TheCompactNavPageFollowsTheLargeTitleState) {
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.compact_chat_open = false;
  state.nav_tab = NavTab::Contacts;
  const std::string rml = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_TRUE(Has(rml, "id=\"shell-nav-page\" data-class-large-title-collapsed=\"large_title_collapsed\""));
}

TEST(ShellLayoutRmlTest, AnActiveDialogIsRenderedByItsMountOnlyNotInlineInTheRoot) {
  // A full layout sync while a dialog is open rebuilds the root and then fills the dialog mount;
  // an inline copy in the root would survive dialog_close as a dead OK button over the whole app.
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.dialog.active = true;
  state.dialog.kind = OverlayKind::Confirm;
  state.dialog.show_cancel = true;
  const ShellLayoutRml rml(state, false, "");
  EXPECT_TRUE(Has(rml.Dialog(), "shell-layer-dialog"));
  EXPECT_FALSE(Has(rml.ShellRoot(), "shell-layer-dialog"));
  EXPECT_TRUE(Has(rml.ShellRoot(), "id=\"shell-dialog-mount\""));
}

TEST(ShellLayoutRmlTest, ADestructiveConfirmCarriesTheDangerClassAndASplitButtonPair) {
  // The buttons' default labels are words ("OK", "Cancel"); with no strings loaded they would be the long keys and
  // stack, so the result would depend on which test loaded the strings first.
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  ShellState state = WithChatPane(LayoutMode::Compact);
  state.dialog.active = true;
  state.dialog.kind = OverlayKind::Confirm;
  state.dialog.show_cancel = true;
  const std::string plain = ShellLayoutRml(state, false, "").Dialog();
  EXPECT_FALSE(Has(plain, "shell-dialog--destructive"));
  EXPECT_TRUE(Has(plain, "shell-dialog-ok shell-dialog-ok--split"));
  EXPECT_FALSE(Has(plain, "shell-dialog-actions--stack"));

  state.dialog.destructive = true;
  EXPECT_TRUE(Has(ShellLayoutRml(state, false, "").Dialog(), "shell-dialog--destructive"));

  // An alert has the one button, with no vertical hairline beside it.
  state.dialog.show_cancel = false;
  EXPECT_FALSE(Has(ShellLayoutRml(state, false, "").Dialog(), "shell-dialog-ok--split"));
}

TEST(ShellLayoutRmlTest, ALongButtonLabelStacksTheDialogButtons) {
  EXPECT_FALSE(ShellLayoutRml::DialogButtonsStacked("Cancel", "Delete"));
  EXPECT_FALSE(ShellLayoutRml::DialogButtonsStacked("取消", "移除联系人"));
  EXPECT_TRUE(ShellLayoutRml::DialogButtonsStacked("Cancel", "Remove this device"));
  EXPECT_TRUE(ShellLayoutRml::DialogButtonsStacked("取消", "移除这台设备并退出登录"));

  ShellState state = WithChatPane(LayoutMode::Compact);
  state.dialog.active = true;
  state.dialog.show_cancel = true;
  state.dialog.ok_label = "Remove this device";
  EXPECT_TRUE(Has(ShellLayoutRml(state, false, "").Dialog(), "shell-dialog-actions--stack"));
}

// DEV/PLAN-pin-gate-redesign.md B: the number pad has no input element (no system keyboard, the PIN bound nowhere);
// the field mode keeps its field, which takes the focus as the gate comes up (Kenneth 2026-11-07).
TEST(ShellLayoutRmlTest, ThePinPadHasNoFieldAndTheFieldModeFocusesItsField) {
  ShellState state;
  state.pin_gate.active = true;
  state.pin_gate.unlock_mode = true;
  state.pin_gate.keypad = true;
  const std::string pad = ShellLayoutRml(state, false, "").PinGate();
  EXPECT_FALSE(Has(pad, "<input"));
  EXPECT_FALSE(Has(pad, "pin_gate_pin"));
  EXPECT_FALSE(Has(pad, "pin_gate_submit()")) << "a full PIN submits itself";
  for (int digit = 0; digit <= 9; ++digit) {
    EXPECT_TRUE(Has(pad, "pin_gate_key(" + std::to_string(digit) + ")")) << digit;
  }
  EXPECT_TRUE(Has(pad, "pin_gate_backspace()"));
  EXPECT_TRUE(Has(pad, "pin_gate_ok()"));
  EXPECT_TRUE(Has(pad, "pin_gate_use_text()")) << "the field is always one tap away";
  EXPECT_FALSE(ShellLayoutRml::PinGateFocusesField(state.pin_gate));

  state.pin_gate.keypad = false;
  const std::string field = ShellLayoutRml(state, false, "").PinGate();
  EXPECT_TRUE(Has(field, std::string("id=\"") + ShellLayoutRml::kPinGateFieldId + "\""));
  EXPECT_TRUE(Has(field, "data-value=\"pin_gate_pin\""));
  EXPECT_FALSE(Has(field, "pin_gate_key("));
  EXPECT_TRUE(Has(field, "pin_gate_use_keypad()"));
  EXPECT_TRUE(ShellLayoutRml::PinGateFocusesField(state.pin_gate));

  for (bool PinGateState::*mode : {&PinGateState::chooser_mode, &PinGateState::identity_fork_mode,
                                   &PinGateState::link_paste_mode, &PinGateState::waiting_device_unlock,
                                   &PinGateState::biometric_first}) {
    PinGateState gate = state.pin_gate;
    gate.*mode = true;
    EXPECT_FALSE(ShellLayoutRml::PinGateFocusesField(gate)) << "no PIN field to type in there";
  }
  PinGateState closed;
  EXPECT_FALSE(ShellLayoutRml::PinGateFocusesField(closed));
}

// Face / fingerprint unlock (DEV/PLAN-pin-gate-redesign.md C): the pad's bottom-left key (where no "OK" is due) with
// the face or the fingerprint icon; the field's link by the sensor's name; the hint line when there is one.
TEST(ShellLayoutRmlTest, TheBiometricKeyLinkAndHintFollowTheirFields) {
  ShellState state;
  state.pin_gate.active = true;
  state.pin_gate.unlock_mode = true;
  state.pin_gate.keypad = true;
  const std::string pad = ShellLayoutRml(state, false, "").PinGate();
  EXPECT_TRUE(Has(pad, "data-if=\"pin_gate_biometric_key && !pin_gate_keypad_ok_shown\" "
                       "data-event-click=\"pin_gate_biometric()\""));
  EXPECT_TRUE(Has(pad, "data-if=\"!pin_gate_keypad_ok_shown && !pin_gate_biometric_key\"")) << "else the blank";
  EXPECT_TRUE(Has(pad, "data-if=\"pin_gate_biometric_kind == 'face'\" src=\"../icons/v2/face-id.svg\""));
  EXPECT_TRUE(Has(pad, "data-if=\"pin_gate_biometric_kind != 'face'\" src=\"../icons/v2/fingerprint.svg\""));
  EXPECT_TRUE(Has(pad, "data-if=\"pin_gate_biometric_hint != ''\">{{pin_gate_biometric_hint}}"));
  for (const char* icon : {"icons/v2/face-id.svg", "icons/v2/fingerprint.svg"}) {
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / icon)) << icon;
  }

  state.pin_gate.keypad = false;
  const std::string field = ShellLayoutRml(state, false, "").PinGate();
  EXPECT_FALSE(Has(field, "face-id.svg"));
  for (const char* kind : {"face", "touch", "fingerprint"}) {
    EXPECT_TRUE(Has(field, std::string("pin_gate_biometric_kind == '") + kind + "'")) << kind;
    EXPECT_TRUE(Has(field, Tr(std::string("pin.biometric_link.") + kind))) << kind;
  }
  EXPECT_TRUE(Has(field, "pin_gate_biometric()"));
  EXPECT_TRUE(Has(field, "{{pin_gate_biometric_hint}}"));
}

// Face / fingerprint first: the door opens with the big sensor key and "Enter PIN" -- no pad, no field.
TEST(ShellLayoutRmlTest, ABiometricFirstDoorHasOnlyTheKeyAndEnterPin) {
  ShellState state;
  state.pin_gate.active = true;
  state.pin_gate.unlock_mode = true;
  state.pin_gate.keypad = true;
  state.pin_gate.biometric_first = true;
  const std::string door = ShellLayoutRml(state, false, "").PinGate();
  EXPECT_TRUE(Has(door, "class=\"shell-pin-bio-key\" data-event-click=\"pin_gate_biometric()\""));
  EXPECT_TRUE(Has(door, "data-if=\"pin_gate_biometric_kind == 'face'\" src=\"../icons/v2/face-id.svg\""));
  EXPECT_TRUE(Has(door, "data-if=\"pin_gate_biometric_kind != 'face'\" src=\"../icons/v2/fingerprint.svg\""));
  EXPECT_TRUE(Has(door, "data-event-click=\"pin_gate_use_pin()\">" + Tr("pin.biometric_use_pin")));
  EXPECT_FALSE(Has(door, "shell-pin-keys")) << "no pad yet";
  EXPECT_FALSE(Has(door, "pin_gate_key("));
  EXPECT_FALSE(Has(door, std::string("id=\"") + ShellLayoutRml::kPinGateFieldId)) << "no field yet";
  EXPECT_TRUE(Has(door, "pin_gate_forgot_toggle()")) << "Forgot PIN stays";
}

TEST(ShellLayoutRmlTest, ThePinPadKeysAreAtLeast64dp) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "themes/components.rcss");
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string css = buffer.str();
  const size_t key = css.find(".shell-pin-key {");
  ASSERT_NE(key, std::string::npos);
  EXPECT_EQ(css.find("width: 72dp;\n  height: 72dp;", key), css.find("width:", key));
}

TEST(ShellLayoutRmlTest, NoDialogNoPinGateNoMarkup) {
  const ShellLayoutRml rml(ShellState{}, false, "");
  EXPECT_TRUE(rml.Dialog().empty());
  EXPECT_TRUE(rml.PinGate().empty());
  EXPECT_TRUE(rml.BottomChrome().empty());
}

} // namespace
} // namespace pbr
