// The friend page's one "Share with others" row and its sheet (DEV/PLAN-fewer-taps-2.md commit 4): the text and the
// QR carry the friend's own nickname, never the remark the user keeps; choices that cannot work are not offered.
#include "gui/contacts/ContactsController.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/ContactLocator.h"
#include "lib/people/ContactTypes.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
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

class ContactShareOthersTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }

  void SetUp() override {
    LocalizationService::Instance().SetPreferredLanguage("en");
    Contact full;
    full.id = "c-full";
    full.local.display_name = "Mom (private remark)";
    full.remote.nickname = "Alice";
    full.remote.ids = {{mesh::ContactIdKind::Account, "account:abc", true},
                       {mesh::ContactIdKind::RelayUser, "relay:alice", true},
                       {mesh::ContactIdKind::PeerId, "QmPhone", true}};
    SyncContactMirrors(full);
    contacts_["c-full"] = full;
    Contact relay_only;
    relay_only.id = "c-relay";
    relay_only.remote.nickname = "Bob";
    relay_only.remote.ids = {{mesh::ContactIdKind::RelayUser, "relay:bob", true}};
    SyncContactMirrors(relay_only);
    contacts_["c-relay"] = relay_only;
    Contact bare;
    bare.id = "c-bare";
    bare.local.display_name = "Nobody";
    SyncContactMirrors(bare);
    contacts_["c-bare"] = bare;

    MessagingContactsPorts ports;
    ports.snapshot = []() {
      MessagingView view;
      view.initialized = true;
      return view;
    };
    ports.get_contact = [this](const std::string& id) -> Roe<std::optional<Contact>> {
      const auto it = contacts_.find(id);
      return it == contacts_.end() ? std::optional<Contact>{} : std::optional<Contact>{it->second};
    };
    ports.share_contact = [](const std::string&, const std::string&) -> Roe<void> { return {}; };
    controller_.BindContactsPorts(ports);
    ShellNavigationPorts nav;
    nav.push_layer = [this](const PaneSpec&) { return ++pushed_; };
    controller_.BindShellNavigation(nav);
    ShellFeedbackPorts feedback;
    feedback.show_toast = [this](const std::string& message, ToastDuration) { toasts_.push_back(message); };
    controller_.BindShellFeedback(feedback);
    ContactsAccountPorts account;
    account.load_identity = []() {
      ProfileIdentityView view;
      view.ready = true;
      view.relay_id = "relay:me";
      view.nickname = "Me";
      view.contact_locator = "mine-locator";
      return view;
    };
    controller_.BindAccountPorts(account);
  }

  static std::vector<std::string> Ids(const std::vector<ContextMenuAction>& actions) {
    std::vector<std::string> ids;
    for (const ContextMenuAction& action : actions) {
      ids.push_back(action.id);
    }
    return ids;
  }

  std::map<std::string, Contact> contacts_;
  ContactsController controller_;
  int pushed_ = 0;
  std::vector<std::string> toasts_;
};

TEST_F(ContactShareOthersTest, TheAccountTextNamesTheFriendByHisOwnNicknameNeverTheRemark) {
  controller_.OnSelectContact("c-full");
  const std::string text = controller_.FriendShareText();
  EXPECT_NE(text.find("Alice"), std::string::npos);
  EXPECT_NE(text.find("relay:alice"), std::string::npos);
  EXPECT_NE(text.find("Alice's PP account"), std::string::npos) << "says whose account it is, not 'my'";
  EXPECT_EQ(text.find("Mom"), std::string::npos);
  EXPECT_EQ(text.find("My PP account"), std::string::npos);
  auto locator = FindContactLocator(text);
  ASSERT_TRUE(locator) << locator.error().message;
  EXPECT_EQ(locator->account_id, "account:abc");
  EXPECT_EQ(locator->nickname, "Alice");
  EXPECT_EQ(locator->devices, std::vector<std::string>{"QmPhone"});
}

TEST_F(ContactShareOthersTest, TheSheetOffersOnlyWhatCanWork) {
  controller_.OnSelectContact("c-full");
  EXPECT_EQ(Ids(controller_.ShareFriendActions({})),
            (std::vector<std::string>{"share_friend_account", "share_friend_card", "share_friend_qr"}));
  controller_.OnSelectContact("c-relay"); // an id to type, but no Account ID to build a code from
  EXPECT_EQ(Ids(controller_.ShareFriendActions({})),
            (std::vector<std::string>{"share_friend_account", "share_friend_card"}));
  controller_.OnSelectContact("c-bare"); // nothing to say about the account: only the card (which explains itself)
  EXPECT_EQ(Ids(controller_.ShareFriendActions({})), std::vector<std::string>{"share_friend_card"});
}

TEST_F(ContactShareOthersTest, TheQrChoiceOpensTheFriendsCodeAndMineAfterItShowsMine) {
  controller_.OnSelectContact("c-full");
  controller_.OpenSelectedFriendQr();
  EXPECT_EQ(controller_.QrPageOwner(), ContactsController::QrOwner::Friend);
  EXPECT_EQ(controller_.QrPageName(), "Alice") << "his own nickname, not the remark";
  auto locator = FindContactLocator(controller_.QrPageLocator());
  ASSERT_TRUE(locator);
  EXPECT_EQ(locator->account_id, "account:abc");
  EXPECT_EQ(locator->nickname, "Alice");

  controller_.OpenMyQr();
  EXPECT_EQ(controller_.QrPageOwner(), ContactsController::QrOwner::Mine);
  EXPECT_EQ(controller_.QrPageLocator(), "mine-locator");
  EXPECT_EQ(controller_.QrPageName(), "Me");
}

TEST_F(ContactShareOthersTest, AFriendWithoutAnAccountIdOpensNoCode) {
  controller_.OnSelectContact("c-relay");
  controller_.OpenSelectedFriendQr();
  EXPECT_EQ(pushed_, 0);
  ASSERT_EQ(toasts_.size(), 1u);
  EXPECT_EQ(toasts_[0], Tr("contacts.share.no_account"));
}

TEST(ContactShareOthersMarkupTest, TheFriendPageHasOneShareRowThatOpensTheSheet) {
  const std::string page = ReadAsset("views/contact_detail.rml");
  EXPECT_NE(page.find("data-event-click=\"share_friend()\""), std::string::npos);
  EXPECT_NE(page.find("{{i18n:contacts.share_others.row}}"), std::string::npos);
  size_t rows = 0;
  for (size_t at = page.find("share_friend()"); at != std::string::npos; at = page.find("share_friend()", at + 1)) {
    ++rows;
  }
  EXPECT_EQ(rows, 1u);
}

} // namespace
} // namespace pbr
