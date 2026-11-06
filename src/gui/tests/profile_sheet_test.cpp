// A person opened from a group (DEV/PLAN-ui-batch8-group-pages.md 2b, Kenneth 2026-11-04: "也弹出那个……上托的那个用户
// 信息框，可以向下把它划掉"): on phones their page rises as a 4/5 page sheet over where the user is — no tab or chat
// changes, swiped down the user is back — with the sheets' own title row; the desktop keeps its pane.
#include "gui/contacts/ContactsController.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

struct FakeShell {
  LayoutMode layout = LayoutMode::Compact;
  std::vector<PaneSpec> layers;
  std::vector<int> open;
  std::vector<std::string> transients;
  std::vector<std::string> primary;
  int tab_changes = 0;
  int closes_all = 0;

  ShellNavigationPorts Ports() {
    ShellNavigationPorts ports;
    ports.snapshot = [this]() {
      ShellChromeSnapshot snap;
      snap.layout_mode = layout;
      snap.nav_tab = NavTab::Sessions;
      return snap;
    };
    ports.push_layer = [this](const PaneSpec& spec) {
      layers.push_back(spec);
      open.push_back(static_cast<int>(layers.size()));
      return open.back();
    };
    ports.close_layer = [this](const int id) { std::erase(open, id); };
    ports.has_layer = [this](const int id) { return std::find(open.begin(), open.end(), id) != open.end(); };
    ports.close_page_sheets = [this]() {
      ++closes_all;
      open.clear();
    };
    ports.push_transient = [this](const PaneSpec& spec) { transients.push_back(spec.key); };
    ports.set_primary_pane = [this](const std::string& key) { primary.push_back(key); };
    ports.clear_primary_pane = []() {};
    ports.select_nav_tab = [this](NavTab) { ++tab_changes; };
    return ports;
  }
};

TEST(ProfileSheetTest, AMemberOpensAsALargePageSheetAndNothingUnderItMoves) {
  FakeShell shell;
  ContactsController controller;
  controller.BindShellNavigation(shell.Ports());
  controller.OnShowMemberSheet("acct:ann", "Ann", "");
  ASSERT_EQ(shell.layers.size(), 1u);
  EXPECT_EQ(shell.layers[0].key, "contact_detail");
  EXPECT_TRUE(shell.layers[0].page_sheet);
  EXPECT_EQ(OverlayEntry{}.sheet_detent, SheetDetent::Large);
  EXPECT_TRUE(controller.DetailSheetOpen());
  EXPECT_TRUE(shell.transients.empty()) << "not pushed as a page";
  EXPECT_TRUE(shell.primary.empty());
  EXPECT_EQ(shell.tab_changes, 0) << "the tab stays";

  // A second person replaces the first in the same sheet.
  controller.OnShowMemberSheet("acct:bo", "Bo", "");
  EXPECT_EQ(shell.layers.size(), 1u);
}

TEST(ProfileSheetTest, SwipedDownTheSheetIsForgotten) {
  FakeShell shell;
  ContactsController controller;
  controller.BindShellNavigation(shell.Ports());
  controller.OnShowMemberSheet("acct:ann", "Ann", "");
  shell.open.clear(); // the swipe, the X or the scrim
  controller.Tick();
  EXPECT_FALSE(controller.DetailSheetOpen());
  EXPECT_EQ(shell.tab_changes, 0);
  controller.OnShowMemberSheet("acct:ann", "Ann", "");
  EXPECT_EQ(shell.layers.size(), 2u) << "opens again as a new sheet";
}

TEST(ProfileSheetTest, ClosedWhileNobodyTickedItAnotherPersonStillOpens) {
  // The Contacts tab is not shown, so nothing ticked the controller when the sheet was swiped away.
  FakeShell shell;
  ContactsController controller;
  controller.BindShellNavigation(shell.Ports());
  controller.OnShowMemberSheet("acct:ann", "Ann", "");
  shell.open.clear();
  controller.OnShowMemberSheet("acct:bo", "Bo", "");
  EXPECT_EQ(shell.layers.size(), 2u);
  EXPECT_EQ(shell.open.size(), 1u);
}

TEST(ProfileSheetTest, TheDesktopKeepsItsPane) {
  FakeShell shell;
  shell.layout = LayoutMode::Expanded;
  ContactsController controller;
  controller.BindShellNavigation(shell.Ports());
  controller.OnShowMemberSheet("acct:ann", "Ann", "");
  EXPECT_TRUE(shell.layers.empty());
  EXPECT_FALSE(controller.DetailSheetOpen());
  EXPECT_EQ(shell.primary, (std::vector<std::string>{"contact_detail"}));
}

TEST(ProfileSheetTest, ThePageHasTheSheetsTitleRowOnlyInASheet) {
  const std::string rml = ReadAsset("views/contact_detail.rml");
  EXPECT_NE(rml.find("class=\"sheet-header cd-sheet-header\""), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:contacts.member_info}}"), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:contacts.info}}"), std::string::npos);
  // A member who is not a contact: Add contact always; Message only when it can be sent, else a grey line.
  EXPECT_NE(rml.find("data-event-click=\"add_member_contact()\""), std::string::npos);
  EXPECT_NE(rml.find("data-if=\"!selected.can_message\">{{i18n:contacts.cant_message_yet}}"), std::string::npos);
  const std::string css = ReadAsset("themes/components.rcss");
  EXPECT_NE(css.find(".cd-sheet-header {\n  display: none;"), std::string::npos);
  EXPECT_NE(css.find(".shell-page-sheet .cd-sheet-header {\n  display: flex;"), std::string::npos);
}

// Kenneth 2026-11-04 (ui-mockups/09-sheets-unify ③): the owner, like everyone, gets the person's page; from that group
// it has a "Group admin" card at the bottom. Remove asks first (the picker's confirm), then the page closes and the
// sheets under it stay.
TEST(ProfileSheetTest, TheOwnersAdminCardShowsOnThatPersonsPageAndRemoveClosesIt) {
  FakeShell shell;
  ContactsController controller;
  controller.BindShellNavigation(shell.Ports());
  shell.layers.push_back(PaneSpec{}); // the member list under it
  shell.open.push_back(1);
  controller.OnShowMemberSheet("acct:ann", "Ann", "");
  ASSERT_TRUE(controller.DetailSheetOpen());
  EXPECT_FALSE(controller.GroupAdminShown()) << "nobody's page has the card by itself";

  int reinvited = 0;
  std::function<void()> confirm_then_done;
  GroupAdminCard card;
  card.can_reinvite = true;
  card.reinvite_note = "Not joined yet";
  card.reinvite = [&reinvited]() { ++reinvited; };
  card.can_remove = true;
  card.remove = [&confirm_then_done](std::function<void()> done) { confirm_then_done = std::move(done); };
  controller.SetGroupAdminCard(card);
  EXPECT_TRUE(controller.GroupAdminShown());

  controller.OnGroupAdminReinvite();
  EXPECT_EQ(reinvited, 1);
  EXPECT_TRUE(controller.DetailSheetOpen()) << "re-inviting keeps the page";

  controller.OnGroupAdminRemove();
  ASSERT_TRUE(confirm_then_done) << "asks first";
  EXPECT_TRUE(controller.DetailSheetOpen());
  confirm_then_done(); // confirmed and removed
  EXPECT_FALSE(controller.DetailSheetOpen());
  EXPECT_FALSE(controller.GroupAdminShown());
  EXPECT_EQ(shell.open, (std::vector<int>{1})) << "only the person's page closes";
}

TEST(ProfileSheetTest, OpeningAnotherPersonDropsTheAdminCard) {
  FakeShell shell;
  ContactsController controller;
  controller.BindShellNavigation(shell.Ports());
  controller.OnShowMemberSheet("acct:ann", "Ann", "");
  GroupAdminCard card;
  card.can_remove = true;
  card.remove = [](std::function<void()>) {};
  controller.SetGroupAdminCard(card);
  ASSERT_TRUE(controller.GroupAdminShown());
  controller.OnShowMemberSheet("acct:bo", "Bo", ""); // not from the owner's tap: no card
  EXPECT_FALSE(controller.GroupAdminShown());

  controller.SetGroupAdminCard(card);
  shell.open.clear(); // swiped away
  controller.Tick();
  EXPECT_FALSE(controller.GroupAdminShown());
}

// Kenneth 2026-11-06: the group actions left the bottom of the page; a member's page opened from a group has an
// "In this group" card under the name (role, who added them), and a "Manage…" row only for who may manage.
TEST(ProfileSheetTest, TheInThisGroupCardSitsUnderTheNameAndManageIsOnlyForWhoMayManage) {
  const std::string rml = ReadAsset("views/contact_detail.rml");
  const size_t card = rml.find("data-if=\"group_role_shown\"");
  ASSERT_NE(card, std::string::npos);
  EXPECT_LT(rml.find("{{selected.title}}"), card) << "under the name card";
  EXPECT_LT(card, rml.find("data-event-click=\"start_chat()\""));
  EXPECT_NE(rml.find("{{group_role_text}}"), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:contacts.group_in_group}}"), std::string::npos);
  const size_t manage = rml.find("data-if=\"admin_shown\" data-event-click=\"admin_menu()\"");
  ASSERT_NE(manage, std::string::npos) << "the Manage row only when the viewer may manage";
  EXPECT_GT(manage, card);
  EXPECT_NE(rml.find("{{i18n:contacts.group_manage}}"), std::string::npos);
  // Nothing of the group actions is left as rows on the page.
  EXPECT_EQ(rml.find("data-event-click=\"admin_remove()\""), std::string::npos);
  EXPECT_EQ(rml.find("data-event-click=\"admin_set_admin()\""), std::string::npos);
  EXPECT_EQ(rml.find("data-event-click=\"admin_reinvite()\""), std::string::npos);
}

TEST(ProfileSheetTest, ARoleCardShowsForAnyGroupMemberButManageOnlyWithActions) {
  FakeShell shell;
  ContactsController controller;
  controller.BindShellNavigation(shell.Ports());
  controller.OnShowMemberSheet("acct:ann", "Ann", "");
  EXPECT_FALSE(controller.GroupRoleShown()) << "not from a group: no card";
  GroupAdminCard card;
  card.role_text = "Member · Added by Bo";
  controller.SetGroupAdminCard(card);
  EXPECT_TRUE(controller.GroupRoleShown());
  EXPECT_FALSE(controller.GroupAdminShown()) << "a plain member's viewer cannot manage";
  EXPECT_TRUE(controller.GroupAdminActions().empty());
  controller.OnShowMemberSheet("acct:bo", "Bo", "");
  EXPECT_FALSE(controller.GroupRoleShown());
}

TEST(ProfileSheetTest, TheManageSheetListsTheActionsInOrderWithRemoveLastAndRed) {
  FakeShell shell;
  ContactsController controller;
  controller.BindShellNavigation(shell.Ports());
  controller.OnShowMemberSheet("acct:ann", "Ann", "");
  GroupAdminCard card;
  card.role_text = "Member";
  card.can_set_admin = true;
  card.set_admin = []() {};
  card.can_remove = true;
  card.remove = [](std::function<void()>) {};
  card.reinvite_removed_label = "Re-invite everyone Ann removed…";
  card.reinvite_removed = []() {};
  card.remove_added_label = "Remove everyone Ann added…";
  card.remove_added = []() {};
  controller.SetGroupAdminCard(card);
  ASSERT_TRUE(controller.GroupAdminShown());
  const auto actions = controller.GroupAdminActions();
  ASSERT_EQ(actions.size(), 4u);
  EXPECT_EQ(actions[0].id, "admin_set_admin");
  EXPECT_EQ(actions[1].id, "admin_reinvite_removed");
  EXPECT_EQ(actions[2].id, "admin_remove_added");
  EXPECT_EQ(actions[3].id, "admin_remove");
  EXPECT_FALSE(actions[0].danger);
  EXPECT_FALSE(actions[1].danger);
  EXPECT_TRUE(actions[3].danger);
}

} // namespace
} // namespace pbr
