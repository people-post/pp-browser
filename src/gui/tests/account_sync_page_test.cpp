// While an imported account's contacts have not come over yet (AccountSyncState), the Contacts page and the Chats list
// show a spinner and one plain line in place of their empty state; a failed or late sync shows the plain "still
// syncing" line; a list with rows, a search that matches nothing and the archive are not touched.
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

/** The opening tag of the first element whose class attribute starts with `cls`. */
std::string TagOf(const std::string& rml, const std::string& cls) {
  const size_t at = rml.find("class=\"" + cls);
  if (at == std::string::npos) {
    return {};
  }
  const size_t open = rml.rfind('<', at);
  return rml.substr(open, rml.find('>', at) - open);
}

/** The element's text up to its closing </div>: its lines. */
std::string BlockOf(const std::string& rml, const std::string& cls) {
  const size_t at = rml.find("class=\"" + cls);
  if (at == std::string::npos) {
    return {};
  }
  return rml.substr(at, rml.find("</div>\n", rml.find("</div>", at) + 1) - at);
}

void ExpectSyncBlock(const std::string& rml, const std::string& empty_class, const std::string& list_empty,
                     const std::string& filter) {
  const std::string tag = TagOf(rml, "sync-pending\"");
  ASSERT_FALSE(tag.empty()) << "the sync block is there";
  EXPECT_NE(tag.find(list_empty), std::string::npos) << "only while the list has no rows";
  EXPECT_NE(tag.find(filter), std::string::npos) << "not while searching (or looking at the archive)";
  EXPECT_NE(tag.find("account_sync_syncing || account_sync_delayed"), std::string::npos);
  const std::string block = BlockOf(rml, "sync-pending\"");
  EXPECT_NE(block.find("class=\"sync-pending-spinner\" data-if=\"account_sync_syncing\""), std::string::npos)
      << "the spinner only while syncing: a late sync stops it";
  EXPECT_NE(block.find("{{i18n:account_sync.pending}}"), std::string::npos);
  EXPECT_NE(block.find("{{i18n:account_sync.delayed}}"), std::string::npos);
  // The normal empty state steps aside for both states.
  const std::string empty_tag = TagOf(rml, empty_class);
  ASSERT_FALSE(empty_tag.empty());
  EXPECT_NE(empty_tag.find("!account_sync_syncing && !account_sync_delayed"), std::string::npos);
}

TEST(AccountSyncPageTest, TheContactsPageShowsTheIndicatorInPlaceOfNoContactsYet) {
  ExpectSyncBlock(ReadAsset("views/contacts.rml"), "contacts-empty", "contacts.size == 0", "search_query == ''");
}

TEST(AccountSyncPageTest, TheChatsListShowsTheIndicatorInPlaceOfNoChatsYet) {
  ExpectSyncBlock(ReadAsset("views/sidebar.rml"), "sidebar-empty", "sessions.size == 0",
                  "sessions_query == '' && !showing_archived");
}

TEST(AccountSyncPageTest, TheSpinnerIsAnimatedAndColouredInBothThemes) {
  const std::string css = ReadAsset("themes/components.rcss");
  EXPECT_NE(css.find(".sync-pending-spinner {"), std::string::npos);
  EXPECT_NE(css.find("animation: 0.9s linear infinite sync-pending-spin;"), std::string::npos);
  EXPECT_NE(css.find("@keyframes sync-pending-spin"), std::string::npos);
  EXPECT_NE(ReadAsset("themes/colors-light.rcss").find(".sync-pending-spinner {"), std::string::npos);
  EXPECT_NE(ReadAsset("themes/colors-dark.rcss").find(".sync-pending-spinner {"), std::string::npos);
}

TEST(AccountSyncPageTest, TheBothModelsBindTheTwoFlags) {
  for (const char* source : {"src/gui/contacts/ContactsController.cpp", "src/gui/chat/ChatDataModel.cpp"}) {
    std::ifstream in(std::filesystem::path(PP_SOURCE_DIR) / source);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    EXPECT_NE(text.find("\"account_sync_syncing\""), std::string::npos) << source;
    EXPECT_NE(text.find("\"account_sync_delayed\""), std::string::npos) << source;
  }
}

std::string ReadSource(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_SOURCE_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string FunctionBody(const std::string& source, const std::string& signature) {
  const size_t begin = source.find(signature);
  if (begin == std::string::npos) {
    return {};
  }
  return source.substr(begin, source.find("\n}\n", begin) - begin);
}

// A sync that can never run (no recovery seed, not registered, signed out) is reported, so the indicator does not wait
// for it forever; one that only cannot run yet (messaging not up) reports nothing and stays pending.
TEST(AccountSyncPageTest, TheHubReportsASyncThatCanNeverRunAndTheAppEndsTheIndicator) {
  const std::string hub =
      FunctionBody(ReadSource("capabilities/messaging/src/feature/conversations/ConversationsHub.cpp"),
                   "void ConversationsHub::PublishAccountLockerSoon() {");
  ASSERT_FALSE(hub.empty());
  const size_t impossible = hub.find("LockerSyncImpossible(");
  ASSERT_NE(impossible, std::string::npos);
  EXPECT_NE(hub.find("report(AccountSyncResult::CannotSync);", impossible), std::string::npos);
  EXPECT_LT(impossible, hub.find("account_locker_->Publish()"));

  const std::string app = ReadSource("src/app/Application.cpp");
  const std::string on_result = FunctionBody(app, "void Application::OnAccountSyncResult(");
  const size_t cannot = on_result.find("AccountSyncResult::CannotSync");
  ASSERT_NE(cannot, std::string::npos);
  EXPECT_NE(on_result.find("account_sync_.OnSyncImpossible();", cannot), std::string::npos);
  EXPECT_NE(on_result.find("PersistAccountSyncPending(false);", cannot), std::string::npos);
}

// Reset my profile makes the old account's timers stale (the 60-second "still syncing" and the retries).
TEST(AccountSyncPageTest, AProfileResetMakesTheOldAccountsSyncTimersStale) {
  const std::string app = ReadSource("src/app/Application.cpp");
  EXPECT_NE(
      FunctionBody(app, "Roe<void> Application::ResetActiveProfile() {").find("account_sync_.ResetForNewProfile();"),
      std::string::npos);
  const std::string arm = FunctionBody(app, "void Application::ArmAccountSyncTimeout() {");
  EXPECT_NE(arm.find("token = account_sync_.TimerToken()"), std::string::npos);
  EXPECT_NE(arm.find("account_sync_.OnTimeout(token);"), std::string::npos);
  EXPECT_NE(FunctionBody(app, "void Application::OnAccountSyncResult(").find("token == account_sync_.TimerToken()"),
            std::string::npos)
      << "a retry armed before the reset does not publish for the new account";
}

TEST(AccountSyncPageTest, TheLinesAreTranslatedAndPlain) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("account_sync.pending"), "正在同步你的联系人和群…");
  EXPECT_EQ(Tr("account_sync.delayed"), "同步还没完成，稍后会自动再试。");
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("account_sync.pending"), "Syncing your contacts and groups…");
  EXPECT_EQ(Tr("account_sync.delayed"), "Still syncing. It will try again on its own.");
}

} // namespace
} // namespace pbr
