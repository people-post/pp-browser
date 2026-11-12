// "Refresh account info…" works for a contact that has an Account ID but no relay id (added from a QR code): it looks
// the person up by Account ID, and an answer for another account is refused before any key is taken or anything saved.
#include "foundation/i18n/LocalizationService.h"
#include "gui/UserFeedback.h"
#include "gui/contacts/ContactsController.h"
#include "lib/people/ContactTypes.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

class ContactRefreshTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }

  void SetUp() override {
    LocalizationService::Instance().SetPreferredLanguage("en");
    contact_.id = "c-ada";
    contact_.remote.nickname = "Ada";
    contact_.remote.ids = {{mesh::ContactIdKind::Account, "account:ada", true}};
    SyncContactMirrors(contact_);

    MessagingContactsPorts ports;
    ports.snapshot = []() {
      MessagingView view;
      view.initialized = true;
      return view;
    };
    ports.get_contact = [this](const std::string&) -> Roe<std::optional<Contact>> { return std::optional(contact_); };
    ports.list_contacts = [this]() -> Roe<std::vector<Contact>> { return std::vector<Contact>{contact_}; };
    ports.lookup_relay_user = [this](const std::string&) -> Roe<mesh::DirectoryHit> {
      ++by_relay_;
      return answer_;
    };
    ports.lookup_account = [this](const std::string& account_id) -> Roe<mesh::DirectoryHit> {
      ++by_account_;
      asked_ = account_id;
      return answer_;
    };
    ports.register_peer_keys_from_directory = [this](const mesh::DirectoryHit&) { ++keys_registered_; };
    ports.apply_remote_snapshot = [this](const std::string&, const mesh::DirectoryHit&, int64_t,
                                         const std::optional<std::string>& requested) -> Roe<Contact> {
      ++applied_;
      requested_ = requested;
      return contact_;
    };
    controller_.BindContactsPorts(ports);

    ShellFeedbackPorts feedback;
    feedback.show_toast = [this](const std::string& message, ToastDuration) { toasts_.push_back(message); };
    UserFeedback::BindPorts(feedback);

    answer_.account_id = "account:ada";
    answer_.ids = {{mesh::ContactIdKind::Account, "account:ada", true},
                   {mesh::ContactIdKind::RelayUser, "relay:ada", false}};
  }
  void TearDown() override {
    UserFeedback::BindPorts({});
    LocalizationService::Instance().SetPreferredLanguage("en");
  }

  Contact contact_;
  ContactsController controller_;
  mesh::DirectoryHit answer_;
  int by_relay_ = 0;
  int by_account_ = 0;
  int keys_registered_ = 0;
  int applied_ = 0;
  std::string asked_;
  std::optional<std::string> requested_;
  std::vector<std::string> toasts_;
};

TEST_F(ContactRefreshTest, AContactWithOnlyAnAccountIdCanBeRefreshed) {
  const ContactsController::ContactDetail detail = ToContactDetail(contact_, {});
  EXPECT_FALSE(detail.has_relay_id);
  EXPECT_TRUE(detail.can_refresh);

  Contact nobody;
  nobody.id = "c-none";
  EXPECT_FALSE(ToContactDetail(nobody, {}).can_refresh);

  std::ifstream in(std::string(PP_BROWSER_ASSETS_DIR) + "/views/contact_detail.rml");
  std::ostringstream rml;
  rml << in.rdbuf();
  EXPECT_NE(rml.str().find("advanced && selected.can_refresh"), std::string::npos);
  EXPECT_EQ(rml.str().find("selected.has_relay_id"), std::string::npos);
}

TEST_F(ContactRefreshTest, RefreshLooksTheContactUpByItsAccountId) {
  controller_.OnSelectContact("c-ada");
  controller_.OnSyncRemote();
  EXPECT_EQ(by_account_, 1);
  EXPECT_EQ(by_relay_, 0);
  EXPECT_EQ(asked_, "account:ada");
  EXPECT_EQ(keys_registered_, 1);
  EXPECT_EQ(applied_, 1);
  EXPECT_EQ(requested_, std::optional<std::string>("account:ada"));
}

TEST_F(ContactRefreshTest, AnAnswerForAnotherAccountRegistersNoKeysAndSavesNothing) {
  answer_.account_id = "account:mallory";
  answer_.ids = {{mesh::ContactIdKind::Account, "account:mallory", true}};
  controller_.OnSelectContact("c-ada");
  controller_.OnSyncRemote();
  EXPECT_EQ(keys_registered_, 0);
  EXPECT_EQ(applied_, 0);
  ASSERT_EQ(toasts_.size(), 1u);
  EXPECT_EQ(toasts_[0], Tr("contacts.err.account_mismatch"));
}

TEST_F(ContactRefreshTest, AnAnswerThatNamesNoAccountIsRefusedToo) {
  answer_.account_id.reset();
  answer_.ids = {{mesh::ContactIdKind::RelayUser, "relay:ada", true}};
  controller_.OnSelectContact("c-ada");
  controller_.OnSyncRemote();
  EXPECT_EQ(keys_registered_, 0);
  EXPECT_EQ(applied_, 0);
}

TEST_F(ContactRefreshTest, ARelayOnlyContactIsLookedUpByItsRelayId) {
  contact_.remote.ids = {{mesh::ContactIdKind::RelayUser, "relay:old", true}};
  SyncContactMirrors(contact_);
  answer_.account_id.reset();
  controller_.OnSelectContact("c-ada");
  controller_.OnSyncRemote();
  EXPECT_EQ(by_relay_, 1);
  EXPECT_EQ(by_account_, 0);
  EXPECT_EQ(applied_, 1);
  EXPECT_FALSE(requested_.has_value());
}

} // namespace
} // namespace pbr
