#include "domain/people/PeerAccountBook.h"

#include "domain/people/MeshHopPolicy.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

class PeerAccountBookTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_browser_peer_account_book_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
            "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    contacts_ = std::make_unique<ContactsStore>(dir_.string());
    book_ = std::make_unique<PeerAccountBook>(*contacts_);
  }
  void TearDown() override {
    book_.reset();
    contacts_.reset();
    std::filesystem::remove_all(dir_);
  }
  void AddContact(const std::string& id, const std::string& account, const std::string& peer_id = {}) {
    Contact contact;
    contact.id = id;
    contact.display_name = id;
    contact.ids = {{ContactIdKind::Account, account, true}};
    if (!peer_id.empty()) {
      contact.ids.push_back({ContactIdKind::PeerId, peer_id, false});
    }
    ASSERT_TRUE(contacts_->Upsert(contact));
  }
  std::string ContactPeerId(const std::string& account) {
    auto found = contacts_->FindByIdentity(account, ContactIdKind::Account);
    return found && found->has_value() ? PeerIdFromContact(**found) : std::string{};
  }

  std::filesystem::path dir_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<PeerAccountBook> book_;
};

TEST_F(PeerAccountBookTest, LearnedPeerIdResolvesBothWaysWithoutAContact) {
  EXPECT_FALSE(book_->Learn("relay:not-an-account", "12D3peer"));
  EXPECT_FALSE(book_->Learn("account:bob", ""));
  ASSERT_TRUE(book_->Learn("account:bob", "12D3bob"));

  auto peer = book_->PeerIdForAccount("account:bob");
  ASSERT_TRUE(peer);
  EXPECT_EQ(*peer, std::optional<std::string>{"12D3bob"});
  auto account = book_->AccountForPeerId("12D3bob", {});
  ASSERT_TRUE(account);
  EXPECT_EQ(*account, std::optional<std::string>{"account:bob"});
}

TEST_F(PeerAccountBookTest, LearningWritesThePeerIdOntoTheContact) {
  AddContact("contact-carol", "account:carol");
  EXPECT_EQ(ContactPeerId("account:carol"), "");
  ASSERT_TRUE(book_->Learn("account:carol", "12D3carol"));
  EXPECT_EQ(ContactPeerId("account:carol"), "12D3carol");

  // A fresh book (restart) still finds it through the contact.
  PeerAccountBook fresh(*contacts_);
  auto peer = fresh.PeerIdForAccount("account:carol");
  ASSERT_TRUE(peer);
  EXPECT_EQ(*peer, std::optional<std::string>{"12D3carol"});
}

TEST_F(PeerAccountBookTest, ContactsAnswerForUnlearnedPeers) {
  AddContact("contact-dan", "account:dan", "12D3dan");
  auto via_candidates = book_->AccountForPeerId("12D3dan", {"account:someone", "account:dan"});
  ASSERT_TRUE(via_candidates);
  EXPECT_EQ(*via_candidates, std::optional<std::string>{"account:dan"});
  auto via_any_contact = book_->AccountForPeerId("12D3dan", {});
  ASSERT_TRUE(via_any_contact);
  EXPECT_EQ(*via_any_contact, std::optional<std::string>{"account:dan"});
  auto unknown = book_->AccountForPeerId("12D3nobody", {"account:dan"});
  ASSERT_TRUE(unknown);
  EXPECT_FALSE(unknown->has_value());
}

} // namespace
} // namespace pbr
