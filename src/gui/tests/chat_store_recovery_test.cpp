#include "gui/ChatStoreRecovery.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

// D105: a chat store that does not open after unlock gets the start-up recovery, worded for why; one that opened
// (created, current, migrated, or reset below its floor — that one is a notice) gets none.

namespace pbr {
namespace {

TEST(ChatStoreRecoveryTest, OnlyAStoreThatDidNotOpenGetsTheRecovery) {
  EXPECT_FALSE(ChatStoreRecoveryFor(std::nullopt).has_value());

  const auto newer = ChatStoreRecoveryFor(ChatStoreProblem::Newer);
  ASSERT_TRUE(newer.has_value());
  EXPECT_EQ(newer->title_key, "chats.recovery.newer_title");
  EXPECT_EQ(newer->message_key, "chats.recovery.newer_message");
  EXPECT_EQ(newer->action_key, "settings.storage.reset_chats");

  const auto failed = ChatStoreRecoveryFor(ChatStoreProblem::NotUpgraded);
  ASSERT_TRUE(failed.has_value());
  EXPECT_EQ(failed->title_key, "chats.recovery.not_upgraded_title");
  EXPECT_EQ(failed->message_key, "chats.recovery.not_upgraded_message");
  EXPECT_EQ(failed->action_key, "settings.storage.reset_chats");
}

// The copy: a newer store tells the user to update instead; both say what a reset keeps; the action leads to a
// confirm, so it ends with "…" (UI_DESIGN_SYSTEM). Every key is in both shipped languages.
TEST(ChatStoreRecoveryTest, TheWordsSayWhatHappenedAndWhatAResetKeeps) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    for (const ChatStoreProblem problem : {ChatStoreProblem::Newer, ChatStoreProblem::NotUpgraded}) {
      const auto prompt = ChatStoreRecoveryFor(problem);
      ASSERT_TRUE(prompt.has_value());
      for (const std::string& key : {prompt->title_key, prompt->message_key, prompt->action_key}) {
        EXPECT_NE(Tr(key), key) << language << ": " << key << " is missing";
      }
      const std::string action = Tr(prompt->action_key);
      EXPECT_TRUE(action.size() >= 3 && action.compare(action.size() - 3, 3, "…") == 0) << action;
    }
  }
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_NE(Tr("chats.recovery.newer_title").find("newer version"), std::string::npos);
  EXPECT_NE(Tr("chats.recovery.newer_message").find("Update the app"), std::string::npos);
  EXPECT_NE(Tr("chats.recovery.not_upgraded_title").find("could not be upgraded"), std::string::npos);
  for (const char* key : {"chats.recovery.newer_message", "chats.recovery.not_upgraded_message",
                          "settings.storage.reset_chats_confirm_message"}) {
    const std::string text = Tr(key);
    EXPECT_NE(text.find("account"), std::string::npos) << key;
    EXPECT_NE(text.find("contacts"), std::string::npos) << key;
    EXPECT_NE(text.find("settings"), std::string::npos) << key;
  }
}

} // namespace
} // namespace pbr
