// The contact page's Refresh (multi-relay R0): what it tells the user, and that every contact's page has it.
#include "foundation/i18n/LocalizationService.h"
#include "gui/contacts/ContactsController.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

class ContactRefreshNoticeTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }
  void SetUp() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_F(ContactRefreshNoticeTest, SaysWhatChanged) {
  ContactRefreshResult result;
  result.devices = ContactRefreshResult::Devices::Answered;
  result.name_changed = true;
  result.devices_changed = true;
  const ContactRefreshNotice notice = DescribeContactRefresh(result);
  EXPECT_TRUE(notice.ok);
  EXPECT_EQ(notice.text, "Got their latest card (name, devices)");

  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(DescribeContactRefresh(result).text, "已拿到对方最新的名片（名字、设备）");
}

TEST_F(ContactRefreshNoticeTest, AnAnswerThatChangedNothingIsUpToDate) {
  ContactRefreshResult result;
  result.directory = ContactRefreshResult::Directory::Answered;
  const ContactRefreshNotice notice = DescribeContactRefresh(result);
  EXPECT_TRUE(notice.ok);
  EXPECT_EQ(notice.text, "Their card is already up to date");
}

TEST_F(ContactRefreshNoticeTest, SaysWhichSourceCouldNotBeReached) {
  ContactRefreshResult result;
  result.devices = ContactRefreshResult::Devices::NotReached;
  result.directory = ContactRefreshResult::Directory::Unreachable;
  EXPECT_FALSE(DescribeContactRefresh(result).ok);
  EXPECT_EQ(DescribeContactRefresh(result).text, "Can't connect right now. Try again later.");
  result.directory = ContactRefreshResult::Directory::NotFound;
  EXPECT_EQ(DescribeContactRefresh(result).text,
            "Couldn't find their card. They may not have updated PP yet — let them know another way.");
  result.directory = ContactRefreshResult::Directory::NotAsked;
  EXPECT_EQ(DescribeContactRefresh(result).text, "They're offline right now. Try again later.");
}

TEST_F(ContactRefreshNoticeTest, KeysThatDidNotCheckOutAreSaidEvenWhenTheRestUpdated) {
  ContactRefreshResult result;
  result.directory = ContactRefreshResult::Directory::Answered;
  result.directory_keys_rejected = true;
  result.photo_changed = true;
  const ContactRefreshNotice notice = DescribeContactRefresh(result);
  EXPECT_FALSE(notice.ok);
  EXPECT_NE(notice.text.find("didn't check out"), std::string::npos) << notice.text;
}

TEST_F(ContactRefreshNoticeTest, EveryContactsPageHasTheButton) {
  std::ifstream in(std::string(PP_BROWSER_ASSETS_DIR) + "/views/contact_detail.rml");
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string rml = ss.str();
  EXPECT_EQ(rml.find("has_relay_id"), std::string::npos) << "not only contacts with a relay id";
  EXPECT_EQ(rml.find("advanced && selected.has_relay_id"), std::string::npos);
  EXPECT_NE(rml.find("refresh_contact()"), std::string::npos);
  EXPECT_NE(rml.find("selected.refreshing"), std::string::npos) << "a busy state while it runs";
}

} // namespace
} // namespace pbr
