// The phone's group info sheet (DEV/PLAN-ui-batch8-group-pages.md 1): a group chat's title opens it as a 4/5 page sheet
// (the small menu stays for direct chats and the desktop); its rows follow ChatPresenter's screen; its buttons run the
// chat's existing actions; it goes when its layer is closed or the chat is left.
#include "foundation/i18n/LocalizationService.h"
#include "gui/chat/GroupInfoSheet.h"
#include "lib/ui/ViewCatalog.h"

#include <gtest/gtest.h>

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

GroupMemberRow Row(const std::string& identity, const std::string& title, const bool owner = false) {
  GroupMemberRow row;
  row.identity = identity;
  row.title = title;
  row.is_owner = owner;
  return row;
}

class FakeHost : public GroupInfoSheetHost {
public:
  bool compact = true;
  GroupInfoScreen screen;
  std::vector<PaneSpec> pushed;
  std::vector<int> open_layers;
  std::vector<std::string> calls;

  bool GroupInfoCompact() const override { return compact; }
  GroupInfoScreen GroupInfoRead(const std::string& thread_id) override {
    calls.push_back("read:" + thread_id);
    return screen.thread_id == thread_id ? screen : GroupInfoScreen{};
  }
  int GroupInfoPushSheet(const PaneSpec& spec) override {
    pushed.push_back(spec);
    open_layers.push_back(static_cast<int>(pushed.size()));
    return open_layers.back();
  }
  void GroupInfoCloseSheet(const int layer_id) override {
    calls.push_back("close:" + std::to_string(layer_id));
    std::erase(open_layers, layer_id);
  }
  bool GroupInfoSheetOpen(const int layer_id) const override {
    return std::find(open_layers.begin(), open_layers.end(), layer_id) != open_layers.end();
  }
  void GroupInfoCall(const std::string& t, const bool video) override {
    calls.push_back(std::string(video ? "video:" : "voice:") + t);
  }
  void GroupInfoMute(const std::string& t, const bool muted) override {
    calls.push_back(std::string(muted ? "mute_menu_with_unmute:" : "mute_menu:") + t);
  }
  void GroupInfoRename(const std::string& t, const bool everyone) override {
    calls.push_back(std::string(everyone ? "rename_everyone:" : "rename_me:") + t);
  }
  void GroupInfoRestoreName(const std::string& t) override { calls.push_back("restore:" + t); }
  void GroupInfoViewAll(const std::string& t) override { calls.push_back("view_all:" + t); }
  void GroupInfoAddMember(const std::string& t) override { calls.push_back("add:" + t); }
  void GroupInfoChoosePreset(const std::string& g, const GroupPreset current) override {
    calls.push_back(std::string("preset:") + g + (current == GroupPreset::Managed ? ":managed" : ":open"));
  }
  void GroupInfoTapMember(const std::string& t, const std::string& id) override {
    calls.push_back("member:" + t + ":" + id);
  }
  void GroupInfoLeave(const std::string& t) override { calls.push_back("leave:" + t); }
  void GroupInfoDissolve(const std::string& g) override { calls.push_back("dissolve:" + g); }

  bool Called(const std::string& call) const { return std::find(calls.begin(), calls.end(), call) != calls.end(); }
};

GroupInfoFacts Facts(const bool owner) {
  GroupInfoFacts facts;
  facts.thread_id = "t1";
  facts.group_id = "g1";
  facts.title = "Weekend hike";
  facts.is_owner = owner;
  facts.self_identity = "acct:me";
  facts.members = {Row("acct:me", "You", owner), Row("acct:a", "Ann"), Row("acct:o", "Olga", !owner)};
  return facts;
}

class GroupInfoSheetTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }

  FakeHost host;
  GroupInfoSheet sheet{host};
};

TEST(GroupInfoSheetChoiceTest, AGroupChatTitleOpensTheSheetOnPhonesOnly) {
  EXPECT_EQ(ChooseChatHeaderSheet(/*compact=*/true, /*group_chat=*/true), ChatHeaderSheet::GroupInfo);
  EXPECT_EQ(ChooseChatHeaderSheet(/*compact=*/true, /*group_chat=*/false), ChatHeaderSheet::Menu);
  EXPECT_EQ(ChooseChatHeaderSheet(/*compact=*/false, /*group_chat=*/true), ChatHeaderSheet::Menu);
  EXPECT_EQ(ChooseChatHeaderSheet(/*compact=*/false, /*group_chat=*/false), ChatHeaderSheet::Menu);
}

TEST_F(GroupInfoSheetTest, OpensAsALargePageSheet) {
  host.screen = BuildGroupInfoScreen(Facts(/*owner=*/true));
  ASSERT_TRUE(sheet.Open("t1"));
  ASSERT_EQ(host.pushed.size(), 1u);
  EXPECT_EQ(host.pushed[0].key, "group_info");
  EXPECT_TRUE(host.pushed[0].page_sheet);
  EXPECT_EQ(OverlayEntry{}.sheet_detent, SheetDetent::Large); // where every new sheet stands
  EXPECT_EQ(ViewCatalog::ResolvePath("group_info"), "views/group_info.rml");
  // Opening it again does not stack a second one.
  ASSERT_TRUE(sheet.Open("t1"));
  EXPECT_EQ(host.pushed.size(), 1u);
}

TEST_F(GroupInfoSheetTest, NothingOnTheDesktopOrForAChatThatIsNotAGroup) {
  host.screen = BuildGroupInfoScreen(Facts(/*owner=*/true));
  host.compact = false;
  EXPECT_FALSE(sheet.Open("t1"));
  host.compact = true;
  EXPECT_FALSE(sheet.Open("direct-1"));
  EXPECT_TRUE(host.pushed.empty());
}

TEST_F(GroupInfoSheetTest, TheViewCountsAndTagsMembers) {
  host.screen = BuildGroupInfoScreen(Facts(/*owner=*/false));
  ASSERT_TRUE(sheet.Open("t1"));
  const GroupInfoView& view = sheet.View();
  EXPECT_EQ(std::string(view.title.c_str()), "Weekend hike");
  EXPECT_EQ(std::string(view.count_label.c_str()), Tr("group_info.people", {{"count", "3"}}));
  EXPECT_EQ(std::string(view.view_all_label.c_str()), Tr("group_info.view_all", {{"count", "3"}}));
  ASSERT_EQ(view.members.size(), 3u);
  EXPECT_EQ(std::string(view.members[0].title.c_str()), "Olga");
  EXPECT_TRUE(view.members[0].owner);
  EXPECT_FALSE(view.members[0].self);
  const auto me = std::find_if(view.members.begin(), view.members.end(),
                               [](const GroupInfoMemberRow& row) { return row.identity == "acct:me"; });
  ASSERT_NE(me, view.members.end());
  EXPECT_TRUE(me->self);
  EXPECT_TRUE(view.show_add_member) << "Open: a member adds people (G019)";
  EXPECT_FALSE(view.show_dissolve);
}

TEST_F(GroupInfoSheetTest, ButtonsRunTheChatsOwnActions) {
  host.screen = BuildGroupInfoScreen(Facts(/*owner=*/true));
  ASSERT_TRUE(sheet.Open("t1"));
  sheet.Call(false);
  sheet.Call(true);
  sheet.Mute();
  sheet.Rename();
  sheet.ViewAll();
  sheet.AddMember();
  sheet.TapMember("acct:a");
  sheet.Leave();
  sheet.Dissolve();
  for (const char* call : {"voice:t1", "video:t1", "mute_menu:t1", "rename_everyone:t1", "view_all:t1", "add:t1",
                           "member:t1:acct:a", "leave:t1", "dissolve:g1"}) {
    EXPECT_TRUE(host.Called(call)) << call;
  }
}

TEST_F(GroupInfoSheetTest, OurOwnRowDoesNothing) {
  host.screen = BuildGroupInfoScreen(Facts(/*owner=*/true));
  ASSERT_TRUE(sheet.Open("t1"));
  sheet.TapMember("acct:me");
  EXPECT_FALSE(host.Called("member:t1:acct:me"));
}

TEST_F(GroupInfoSheetTest, AManagedGroupsMemberRenamesForThemselvesAndHasNoOwnerButtons) {
  GroupInfoFacts facts = Facts(/*owner=*/false);
  facts.preset = GroupPreset::Managed; // G017: Open would let a member add people and rename for everyone
  facts.has_local_title = true;
  host.screen = BuildGroupInfoScreen(facts);
  ASSERT_TRUE(sheet.Open("t1"));
  EXPECT_TRUE(sheet.View().show_restore_name);
  sheet.Rename();
  sheet.RestoreName();
  sheet.AddMember();
  sheet.Dissolve();
  EXPECT_TRUE(host.Called("rename_me:t1"));
  EXPECT_TRUE(host.Called("restore:t1"));
  EXPECT_FALSE(host.Called("add:t1"));
  EXPECT_FALSE(host.Called("dissolve:g1"));
  EXPECT_TRUE(sheet.View().show_preset) << "everyone reads who can add people";
  EXPECT_FALSE(sheet.View().preset_editable);
  sheet.ChoosePreset();
  EXPECT_FALSE(host.Called("preset:g1:managed")) << "only the owner switches it";
}

// G017: the owner's preset row opens the choice, with the current value.
TEST_F(GroupInfoSheetTest, TheOwnerSwitchesWhoCanAddPeople) {
  GroupInfoFacts facts = Facts(/*owner=*/true);
  facts.preset = GroupPreset::Managed;
  host.screen = BuildGroupInfoScreen(facts);
  ASSERT_TRUE(sheet.Open("t1"));
  EXPECT_TRUE(sheet.View().preset_editable);
  EXPECT_EQ(std::string(sheet.View().preset_label.c_str()), Tr("group_info.preset_managed"));
  sheet.ChoosePreset();
  EXPECT_TRUE(host.Called("preset:g1:managed"));
}

// G019: an Open group's member adds people and renames for everyone from the sheet.
TEST_F(GroupInfoSheetTest, AnOpenGroupsMemberAddsAndRenamesForEveryone) {
  host.screen = BuildGroupInfoScreen(Facts(/*owner=*/false));
  ASSERT_TRUE(sheet.Open("t1"));
  sheet.Rename();
  sheet.AddMember();
  EXPECT_TRUE(host.Called("rename_everyone:t1"));
  EXPECT_TRUE(host.Called("add:t1"));
}

TEST_F(GroupInfoSheetTest, MuteShowsItsStateAndOffersUnmuteWhenOn) {
  GroupInfoFacts facts = Facts(/*owner=*/false);
  host.screen = BuildGroupInfoScreen(facts);
  ASSERT_TRUE(sheet.Open("t1"));
  EXPECT_FALSE(sheet.View().muted);
  facts.muted = true;
  host.screen = BuildGroupInfoScreen(facts);
  sheet.Refresh();
  EXPECT_TRUE(sheet.View().muted);
  sheet.Mute();
  EXPECT_TRUE(host.Called("mute_menu_with_unmute:t1"));
}

TEST_F(GroupInfoSheetTest, ADissolvedGroupShowsOnlyItsNameAndMute) {
  GroupInfoFacts facts = Facts(/*owner=*/true);
  facts.dissolved = true;
  host.screen = BuildGroupInfoScreen(facts);
  ASSERT_TRUE(sheet.Open("t1"));
  const GroupInfoView& view = sheet.View();
  EXPECT_TRUE(view.dissolved);
  EXPECT_EQ(std::string(view.count_label.c_str()), "");
  EXPECT_FALSE(view.show_members);
  EXPECT_FALSE(view.show_calls);
  EXPECT_FALSE(view.show_rename);
  EXPECT_FALSE(view.show_add_member);
  EXPECT_FALSE(view.show_leave);
  EXPECT_FALSE(view.show_dissolve);
  EXPECT_TRUE(view.members.empty());
  sheet.Call(false);
  sheet.Leave();
  EXPECT_FALSE(host.Called("voice:t1"));
  EXPECT_FALSE(host.Called("leave:t1"));
}

TEST_F(GroupInfoSheetTest, ClosedByASwipeItForgetsItsLayer) {
  host.screen = BuildGroupInfoScreen(Facts(/*owner=*/true));
  ASSERT_TRUE(sheet.Open("t1"));
  host.open_layers.clear(); // the X, the scrim or a swipe
  sheet.Tick("t1", std::chrono::steady_clock::now());
  EXPECT_FALSE(sheet.IsOpen());
  ASSERT_TRUE(sheet.Open("t1"));
  EXPECT_EQ(host.pushed.size(), 2u);
}

TEST_F(GroupInfoSheetTest, LeavingTheChatClosesTheSheet) {
  host.screen = BuildGroupInfoScreen(Facts(/*owner=*/true));
  ASSERT_TRUE(sheet.Open("t1"));
  sheet.Tick("t1", std::chrono::steady_clock::now());
  EXPECT_TRUE(sheet.IsOpen());
  sheet.Tick("direct-9", std::chrono::steady_clock::now()); // "Message" from a member opened another chat
  EXPECT_FALSE(sheet.IsOpen());
  EXPECT_TRUE(host.open_layers.empty());
}

TEST(GroupInfoViewTest, TheMarkupUsesTheSharedSheetHeaderCardsAndRows) {
  const std::string rml = ReadAsset("views/group_info.rml");
  for (const char* part : {"class=\"sheet-header\"", "class=\"sheet-title\"", "data-model=\"group_info\"",
                           "class=\"cd-card", "class=\"cd-row cd-row--danger\"", "data-event-click=\"group_info_leave()\"",
                           "data-event-click=\"group_info_dissolve()\"", "group_info_member(member.identity)",
                           "data-class-cd-act--on=\"muted\""}) {
    EXPECT_NE(rml.find(part), std::string::npos) << part;
  }
  // Colours come from the theme: none written in the view.
  EXPECT_EQ(rml.find('#'), std::string::npos);
}

} // namespace
} // namespace pbr
