// Groups page (the owner, after linking a device, could not find the account's groups): Contacts and New chat each have
// a "Groups" row with the count, opening the list of the groups this account is in; a tap opens the group's chat, a long
// press gives the Chats list's own menu for it.
#include "common/runtime/AppRuntime.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/contacts/ContactsController.h"
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

std::string ReadSource(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_SOURCE_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

GroupListEntry Entry(const std::string& thread_id, const std::string& title, const size_t members) {
  GroupListEntry entry;
  entry.thread_id = thread_id;
  entry.title = title;
  entry.stable_id = "group:" + thread_id;
  entry.member_count = members;
  return entry;
}

class GroupListPageTest : public ::testing::Test {
protected:
  void SetUp() override {
    ContactsController::InstallInstance(controller_);
    AppRuntime::Initialize(AppRuntimeConfig{});
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");

    ShellNavigationPorts nav;
    nav.snapshot = [this]() {
      ShellChromeSnapshot chrome;
      chrome.layout_mode = LayoutMode::Compact;
      chrome.nav_tab = shown_tab_;
      return chrome;
    };
    nav.push_transient = [this](const PaneSpec& spec) { pages_.push_back(spec.key); };
    nav.pop_transient = [this]() {
      if (!pages_.empty()) {
        pages_.pop_back();
      }
    };
    nav.select_nav_tab = [this](NavTab tab) { tab_ = tab; };
    nav.set_primary_pane = [this](const std::string& key) { pane_ = key; };
    controller_.BindShellNavigation(nav);

    MessagingContactsPorts ports;
    ports.snapshot = []() {
      MessagingView view;
      view.initialized = true;
      return view;
    };
    ports.list_contacts = []() -> Roe<std::vector<Contact>> { return std::vector<Contact>{}; };
    ports.list_groups = [this]() -> Roe<std::vector<GroupListEntry>> {
      ++group_reads_;
      return groups_;
    };
    controller_.BindContactsPorts(ports);

    ChatSessionPorts chat;
    chat.select_thread = [this](const std::string& thread_id) { opened_.push_back(thread_id); };
    controller_.BindChatPorts(chat);
    controller_.OnNavTabActivated();
  }
  void TearDown() override {
    controller_.OnGroupListDismissed();
    controller_.OnNewChatDismissed();
    controller_.BindChatPorts({});
    controller_.BindContactsPorts({});
    controller_.BindShellNavigation({});
    AppRuntime::Shutdown();
    ContactsController::ClearInstance();
  }

  ContactsController controller_;
  std::vector<GroupListEntry> groups_;
  std::vector<std::string> pages_;
  std::vector<std::string> opened_;
  NavTab tab_ = NavTab::Contacts;
  std::string pane_;
  NavTab shown_tab_ = NavTab::Contacts; // the tab the shell shows (the snapshot's)
  int group_reads_ = 0;                 // list_groups calls: each reads every group from the store
};

TEST(GroupListRowsTest, RowsAreByNameWithTheirMemberCount) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("en");
  const auto rows = GroupListRowsByName({Entry("t2", "Walkers", 5), Entry("t1", "Book club", 3)});
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(std::string(rows[0].thread_id.c_str()), "t1");
  EXPECT_EQ(std::string(rows[0].title.c_str()), "Book club");
  EXPECT_EQ(std::string(rows[0].members.c_str()), "3 members");
  EXPECT_EQ(std::string(rows[1].thread_id.c_str()), "t2");
  EXPECT_NE(std::string(rows[0].avatar_letter.c_str()), "?");
}

TEST_F(GroupListPageTest, TheRowsCountAndThePageListsEveryGroup) {
  groups_ = {Entry("t1", "Book club", 3), Entry("t2", "Walkers", 5)};
  controller_.Refresh();
  controller_.OpenGroupList();
  ASSERT_EQ(pages_, (std::vector<std::string>{"group_list"}));
  controller_.OpenGroupList();
  EXPECT_EQ(pages_.size(), 1u) << "pushed once";
}

TEST_F(GroupListPageTest, ATapOpensTheGroupsChatAndClosesThePages) {
  groups_ = {Entry("t1", "Book club", 3)};
  controller_.OpenNewChatPage();
  controller_.OpenGroupList();
  ASSERT_EQ(pages_, (std::vector<std::string>{"new_chat", "group_list"}));
  controller_.OnGroupListOpen("t1");
  EXPECT_EQ(opened_, (std::vector<std::string>{"t1"}));
  EXPECT_TRUE(pages_.empty()) << "the Groups page and New chat under it are gone";
  EXPECT_EQ(tab_, NavTab::Sessions);
  EXPECT_EQ(pane_, "chat");
}

// A contacts refresh runs at every incoming message: with neither the Groups page, New chat nor the Contacts tab on
// screen it reads no groups.
TEST_F(GroupListPageTest, ARefreshWithThePagesClosedReadsNoGroups) {
  groups_ = {Entry("t1", "Book club", 3)};
  shown_tab_ = NavTab::Sessions;
  group_reads_ = 0;
  controller_.Refresh();
  controller_.OnContactsChanged();
  controller_.OnThreadsChanged();
  EXPECT_EQ(group_reads_, 0);

  shown_tab_ = NavTab::Contacts; // the Groups row's count is on screen again
  controller_.OnNavTabActivated();
  EXPECT_EQ(group_reads_, 1);
  EXPECT_EQ(controller_.GroupListCountForTest(), 1);
  controller_.Refresh();
  EXPECT_EQ(group_reads_, 2) << "while Contacts shows, a refresh keeps the count current";

  shown_tab_ = NavTab::Sessions;
  controller_.OpenNewChatPage(); // New chat's Groups row
  const int before = group_reads_;
  controller_.Refresh();
  EXPECT_EQ(group_reads_, before + 1);
}

// Left (or deleted) from the Groups page's long-press menu: the chats change, and the open page drops the group.
TEST_F(GroupListPageTest, TheOpenPageDropsAGroupLeftFromItsMenu) {
  groups_ = {Entry("t1", "Book club", 3), Entry("t2", "Walkers", 5)};
  controller_.OpenGroupList();
  ASSERT_EQ(controller_.GroupListRowsForTest().size(), 2u);
  groups_ = {Entry("t2", "Walkers", 5)}; // "Leave group" on Book club
  controller_.OnThreadsChanged();
  ASSERT_EQ(controller_.GroupListRowsForTest().size(), 1u);
  EXPECT_EQ(std::string(controller_.GroupListRowsForTest()[0].thread_id.c_str()), "t2");
  EXPECT_EQ(controller_.GroupListCountForTest(), 1);
}

TEST(GroupListMarkupTest, TheChatsTellContactsWhenTheyChange) {
  const std::string chat = ReadSource("src/gui/chat/ChatController.cpp");
  const size_t refresh = chat.find("void ChatController::RefreshFromMessaging() {");
  ASSERT_NE(refresh, std::string::npos);
  const size_t told = chat.find("contacts_notify_.threads_changed();", refresh);
  ASSERT_NE(told, std::string::npos);
  EXPECT_LT(told, chat.find("\n}\n", refresh));
  const std::string app = ReadSource("src/app/Application.cpp");
  EXPECT_NE(app.find("contacts_notify.threads_changed = [this]() { contacts_->OnThreadsChanged(); };"),
            std::string::npos);
}

TEST(GroupListMarkupTest, ThePageIsRegisteredAndBindsItsRows) {
  EXPECT_EQ(ViewCatalog::ResolvePath("group_list"), "views/group_list.rml");
  const std::string rml = ReadAsset("views/group_list.rml");
  EXPECT_NE(rml.find("data-model=\"contacts\""), std::string::npos);
  EXPECT_NE(rml.find("data-for=\"row : group_list_rows\""), std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"group_list_open(row.thread_id)\""), std::string::npos);
  EXPECT_NE(rml.find("data-attr-session-id=\"row.thread_id\""), std::string::npos)
      << "the long press is the Chats list's menu for the group";
  EXPECT_NE(rml.find("{{row.members}}"), std::string::npos);
  EXPECT_NE(rml.find("data-if=\"group_list_rows.size == 0\""), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:groups.empty}}"), std::string::npos);
}

TEST(GroupListMarkupTest, ContactsAndNewChatHaveAGroupsRowWithTheCount) {
  for (const char* view : {"views/contacts.rml", "views/new_chat.rml"}) {
    const std::string rml = ReadAsset(view);
    const size_t row = rml.find("data-event-click=\"open_group_list()\"");
    ASSERT_NE(row, std::string::npos) << view;
    EXPECT_NE(rml.find("{{i18n:groups.title}}", row), std::string::npos) << view;
    EXPECT_NE(rml.find("{{group_list_count}}", row), std::string::npos) << view;
  }
  // On Contacts it sits above the A-Z list.
  const std::string contacts = ReadAsset("views/contacts.rml");
  EXPECT_LT(contacts.find("open_group_list()"), contacts.find("data-for=\"section : contact_sections\""));
}

TEST(GroupListMarkupTest, StringsAreInBothLanguagesAndRowsAreBigEnough) {
  const std::string en = ReadAsset("locales/en.json");
  const std::string zh = ReadAsset("locales/zh-Hans.json");
  EXPECT_NE(en.find("\"groups.title\": \"Groups\""), std::string::npos);
  EXPECT_NE(en.find("\"groups.empty\": \"No groups yet\""), std::string::npos);
  EXPECT_NE(zh.find("\"groups.title\": \"群聊\""), std::string::npos);
  EXPECT_NE(zh.find("\"groups.empty\": \"还没有群聊\""), std::string::npos);
  const std::string css = ReadAsset("themes/components.rcss");
  EXPECT_NE(css.find("height: 60dp;", css.find(".blocked-row {")), std::string::npos) << "the page's rows";
  EXPECT_NE(css.find("height: 56dp;", css.find(".nc-action {")), std::string::npos) << "the Groups rows";
}

TEST(GroupListMarkupTest, TheShellTellsTheControllerWhenThePageGoes) {
  const std::string app = ReadSource("src/app/Application.cpp");
  const size_t key = app.find("if (key == \"group_list\") {");
  ASSERT_NE(key, std::string::npos);
  EXPECT_NE(app.find("contacts.OnGroupListDismissed();", key), std::string::npos);
}

} // namespace
} // namespace pbr
