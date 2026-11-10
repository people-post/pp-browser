// A local action's toast (added a contact, joined a group...) in the user's language.
#include "gui/chat/ActionNoticeText.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

class ActionNoticeTextTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_F(ActionNoticeTextTest, ALocalActionIsWordedInTheUiLanguage) {
  ActionNotice notice;
  notice.kind = ActionNotice::Kind::AddedContact;
  notice.name = "Ann";
  EXPECT_EQ(ActionNoticeText(notice), "Added Ann to contacts");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  const std::string zh = ActionNoticeText(notice);
  EXPECT_EQ(zh.find("Added"), std::string::npos) << zh;
  EXPECT_NE(zh.find("Ann"), std::string::npos) << zh;
  for (const auto kind :
       {ActionNotice::Kind::OpenedConversation, ActionNotice::Kind::OpenedSecureConversation,
        ActionNotice::Kind::OpenedThread, ActionNotice::Kind::Registered, ActionNotice::Kind::RegistrationFailed,
        ActionNotice::Kind::JoinedGroup, ActionNotice::Kind::DeclinedInvitation, ActionNotice::Kind::BlockedInviter,
        ActionNotice::Kind::BlockedUnknownInviter, ActionNotice::Kind::CreatedGroup, ActionNotice::Kind::StartedFresh,
        ActionNotice::Kind::OpenedOwnerChat}) {
    notice.kind = kind;
    const std::string text = ActionNoticeText(notice);
    EXPECT_FALSE(text.empty());
    EXPECT_EQ(text.find("chat.action."), std::string::npos) << "a missing key answers with itself";
  }
}

} // namespace
} // namespace pbr
