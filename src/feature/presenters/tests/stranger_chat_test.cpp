// A first message from someone new: the title is "New contact (name)" with no "~" and no account string, and the bar
// (Add / Block) shows only for a chat with a person who is not a contact.
#include "feature/presenters/StrangerChat.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

class StrangerChatTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

PeerDisplayLabel Unknown(const std::string& name) {
  PeerDisplayLabel label;
  label.title = name.empty() ? "account:2OC9…" : "~" + name + " @account:2OC9…";
  label.peer_name = name;
  label.trust = name.empty() ? PeerLabelTrust::RawId : PeerLabelTrust::DirectoryUnverified;
  return label;
}

TEST_F(StrangerChatTest, TheTitleHasTheNameOrNone) {
  EXPECT_EQ(StrangerChatTitle(Unknown("user1")), "新朋友（user1）");
  EXPECT_EQ(StrangerChatTitle(Unknown("")), "新朋友");
  EXPECT_EQ(StrangerChatTitle(Unknown("user1")).find('~'), std::string::npos);
  EXPECT_EQ(StrangerChatTitle(Unknown("user1")).find("account"), std::string::npos);
}

TEST_F(StrangerChatTest, TheBarSaysTheyAreNotAContactYet) {
  EXPECT_EQ(StrangerBarText(Unknown("user1")), "user1 还不是你的联系人。");
  EXPECT_EQ(StrangerBarText(Unknown("")), "这个人还不是你的联系人。");
  EXPECT_EQ(Tr("chat.stranger.add"), "添加");
  EXPECT_EQ(Tr("chat.stranger.block"), "屏蔽");
}

TEST_F(StrangerChatTest, OnlyAPersonWhoIsNotAContactIsAStranger) {
  PeerDisplayLabel label = Unknown("user1");
  EXPECT_TRUE(IsStrangerDirectChat(ThreadKind::Direct, label));
  EXPECT_FALSE(IsStrangerDirectChat(ThreadKind::Group, label));
  EXPECT_FALSE(IsStrangerDirectChat(ThreadKind::Ai, label));
  // Added (or blocked: a block is a contact record): the bar goes and the plain name shows.
  label.contact_id = "c1";
  label.trust = PeerLabelTrust::Contact;
  EXPECT_FALSE(IsStrangerDirectChat(ThreadKind::Direct, label));
}

} // namespace
} // namespace pbr
