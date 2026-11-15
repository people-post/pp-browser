// The contact page's Refresh (multi-relay R0, R3): what it tells the user, that every contact's page has it, and the
// preferred relay line.
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

using Ask = ContactRefreshResult::RelayAsk;

TEST_F(ContactRefreshNoticeTest, SaysWhatChanged) {
  ContactRefreshResult result;
  result.devices = ContactRefreshResult::Devices::Answered;
  result.relays = {{"https://a.example", Ask::Whose::Theirs, Ask::Outcome::Unreachable},
                   {"https://b.example", Ask::Whose::Theirs, Ask::Outcome::Answered}};
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
  result.relays = {{"https://ours.example", Ask::Whose::Ours, Ask::Outcome::Answered}};
  const ContactRefreshNotice notice = DescribeContactRefresh(result);
  EXPECT_TRUE(notice.ok);
  EXPECT_EQ(notice.text, "Their card is already up to date");
}

TEST_F(ContactRefreshNoticeTest, SaysWhyNothingAnswered) {
  ContactRefreshResult result;
  result.devices = ContactRefreshResult::Devices::NotReached;
  result.relays = {{"https://a.example", Ask::Whose::Theirs, Ask::Outcome::Unreachable},
                   {"https://b.example", Ask::Whose::Theirs, Ask::Outcome::Unreachable},
                   {"https://ours.example", Ask::Whose::Ours, Ask::Outcome::Unreachable}};
  EXPECT_FALSE(DescribeContactRefresh(result).ok);
  EXPECT_EQ(DescribeContactRefresh(result).text, "Can't connect right now. Try again later.");
  result.relays = {{"https://ours.example", Ask::Whose::Ours, Ask::Outcome::NotFound}};
  EXPECT_EQ(DescribeContactRefresh(result).text,
            "Couldn't find their card. They may not have updated PP yet — let them know another way.");
  result.devices = ContactRefreshResult::Devices::NotAsked;
  EXPECT_EQ(DescribeContactRefresh(result).text,
            "Couldn't find their card. They may not have updated PP yet — let them know another way.");
  result.relays.clear();
  EXPECT_EQ(DescribeContactRefresh(result).text,
            "This contact's card is incomplete. Ask them to send their card or QR code again.");
  result.devices = ContactRefreshResult::Devices::NotReached;
  EXPECT_EQ(DescribeContactRefresh(result).text, "They're offline right now. Try again later.");
}

TEST_F(ContactRefreshNoticeTest, KeysThatDidNotCheckOutAreSaidEvenWhenTheRestUpdated) {
  ContactRefreshResult result;
  result.relays = {{"https://ours.example", Ask::Whose::Ours, Ask::Outcome::Answered}};
  result.relay_keys_rejected = true;
  result.photo_changed = true;
  const ContactRefreshNotice notice = DescribeContactRefresh(result);
  EXPECT_FALSE(notice.ok);
  EXPECT_NE(notice.text.find("didn't check out"), std::string::npos) << notice.text;
}

// Multi-relay R3: the contact page names the preferred relay by its host, with its reachability.
TEST_F(ContactRefreshNoticeTest, ThePageShowsThePreferredRelayByHostWithItsReachability) {
  ContactRelayView view{.list = ContactRelayView::List::Listed, .origin = "https://www.brief.global/api/relay"};
  ContactRelayLine line = DescribeContactRelay(view);
  EXPECT_EQ(line.relay, "www.brief.global");
  EXPECT_EQ(line.reach, "Not known yet") << "until a refresh asked it";
  view.reach = ContactRelayView::Reach::Reachable;
  EXPECT_EQ(DescribeContactRelay(view).reach, "Reachable");
  view.reach = ContactRelayView::Reach::Unreachable;
  EXPECT_EQ(DescribeContactRelay(view).reach, "Couldn't reach");
  view.reach = ContactRelayView::Reach::NotRegistered;
  EXPECT_EQ(DescribeContactRelay(view).reach, "Doesn't have this account");

  line = DescribeContactRelay({.list = ContactRelayView::List::NoneListed});
  EXPECT_EQ(line.relay, "None listed. Mail goes through your relay.") << "our sends go through our relay, not the mesh";
  EXPECT_TRUE(line.reach.empty());
  line = DescribeContactRelay({.list = ContactRelayView::List::NotKnown});
  EXPECT_EQ(line.relay, "Not known yet");
  EXPECT_TRUE(line.reach.empty());

  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(DescribeContactRelay({.list = ContactRelayView::List::NoneListed}).relay, "未列出。消息经由你的中继发送。");
}

TEST_F(ContactRefreshNoticeTest, EveryContactsPageHasTheButton) {
  std::ifstream in(std::string(PP_BROWSER_ASSETS_DIR) + "/views/contact_detail.rml");
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string rml = ss.str();
  EXPECT_EQ(rml.find("has_relay_id"), std::string::npos) << "not only contacts with an id";
  EXPECT_EQ(rml.find("has_account_id"), std::string::npos);
  EXPECT_NE(rml.find("refresh_contact()"), std::string::npos);
  EXPECT_NE(rml.find("selected.refreshing"), std::string::npos) << "a busy state while it runs";
  EXPECT_NE(rml.find("selected.relay_host"), std::string::npos) << "the preferred relay (R3)";
  EXPECT_NE(rml.find("selected.relay_reach"), std::string::npos) << "and its reachability";
}

} // namespace
} // namespace pbr
