// The PP 客服 chat a settings "?" opens (DEV/PLAN-help-to-support-panel.md step 2): when PP 客服 cannot be reached
// the error carries words for the user (the panel shows them; an empty error would show nothing), and the chat is
// found among the threads without being opened.
#include "feature/settings/SupportThread.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

class SupportThreadTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }
};

ClientCompatSupport Support(const bool enabled, const std::string& account_id) {
  ClientCompatSupport support;
  support.enabled = enabled;
  support.account_id = account_id;
  support.display_name = "PP Support";
  return support;
}

Thread Direct(const std::string& id, const std::string& contact_id, const ThreadChannel channel) {
  Thread thread;
  thread.id = id;
  thread.kind = ThreadKind::Direct;
  thread.channel = channel;
  thread.participant_contact_ids = {contact_id};
  return thread;
}

TEST_F(SupportThreadTest, NoConfigurationDisabledOrNoAccountSaysPPSupportCannotBeReached) {
  const std::string words = Tr("settings.support.unavailable");
  ASSERT_FALSE(words.empty());
  for (const std::optional<ClientCompatSupport>& discovery :
       {std::optional<ClientCompatSupport>{}, std::optional{Support(false, "account:support")},
        std::optional{Support(true, "")}}) {
    const Roe<void> checked = CheckSupportConfigured(discovery);
    ASSERT_FALSE(checked);
    EXPECT_EQ(checked.error().user, words);
  }
  EXPECT_TRUE(CheckSupportConfigured(Support(true, "account:support")));
}

TEST_F(SupportThreadTest, AnErrorWithoutWordsGetsTheSupportSentenceAndOneWithWordsKeepsThem) {
  EXPECT_EQ(WithSupportWords(Error("Contact has no messaging identity")).user, Tr("settings.support.unavailable"));
  EXPECT_EQ(WithSupportWords(Error("x").WithUser("their own words")).user, "their own words");
}

TEST_F(SupportThreadTest, TheChatIsTheDirectPublicOneWithTheSupportContact) {
  Thread ai;
  ai.id = "ai";
  ai.kind = ThreadKind::Ai;
  const std::vector<Thread> threads = {ai, Direct("private", "c-support", ThreadChannel::E2e),
                                       Direct("someone", "c-ann", ThreadChannel::E2ePublic),
                                       Direct("support", "c-support", ThreadChannel::E2ePublic)};
  EXPECT_EQ(FindSupportThread(threads, "c-support"), std::optional<std::string>("support"));
  EXPECT_EQ(FindSupportThread(threads, "c-bob"), std::nullopt) << "never made: none";
  EXPECT_EQ(FindSupportThread({}, "c-support"), std::nullopt);
}

} // namespace
} // namespace pbr
