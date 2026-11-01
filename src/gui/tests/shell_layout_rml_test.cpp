#include "gui/shell/ShellLayoutRml.h"

#include <gtest/gtest.h>

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

TEST(ShellLayoutRmlTest, NoDialogNoPinGateNoMarkup) {
  const ShellLayoutRml rml(ShellState{}, false, "");
  EXPECT_TRUE(rml.Dialog().empty());
  EXPECT_TRUE(rml.PinGate().empty());
  EXPECT_TRUE(rml.BottomChrome().empty());
}

} // namespace
} // namespace pbr
