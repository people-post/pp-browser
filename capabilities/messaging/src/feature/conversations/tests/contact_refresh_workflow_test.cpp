#include "feature/conversations/ContactRefreshWorkflow.h"

#include "common/directory/DirectoryJson.h"

#include "lib/messaging/tests/TestAccount.h"
#include "lib/people/AccountCard.h"
#include "lib/people/ContactIdentity.h"
#include "lib/people/testing/InMemoryPeerCards.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/MeshAlias.h"

namespace pbr {
namespace {

constexpr int64_t kNow = 1'761'235'200'000;

/** A directory that writes down what it was asked, in the order the sources were asked. */
class RecordingDirectory : public mesh::IDirectoryClient {
public:
  explicit RecordingDirectory(std::vector<std::string>& asked) : asked_(asked) {}
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string&) override {
    return std::vector<mesh::DirectoryHit>{};
  }
  Roe<mesh::DirectoryHit> LookupRelayUser(const std::string& relay_user_id) override {
    asked_.push_back("directory:relay:" + relay_user_id);
    return answer;
  }
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& account_id) override {
    asked_.push_back("directory:account:" + account_id);
    return answer;
  }

  Roe<mesh::DirectoryHit> answer = Error("Account not found");

private:
  std::vector<std::string>& asked_;
};

class ContactRefreshWorkflowTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_contact_refresh_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    contacts_ = std::make_unique<ContactsStore>(dir_.string());
    cards_ = std::make_unique<InMemoryPeerCards>();
    keys_ = std::make_unique<AccountKeyResolver>(signing_, kem_, *cards_, directory_);
    exchange_ = std::make_unique<ContactCardExchange>(*contacts_, *keys_);
    // A device answers only when the test gave it a card.
    exchange_->SetCardFetcher([this](const std::string& peer_id, const std::string& account_id,
                                     std::function<void(Roe<PeerCardRecord>)> done) {
      asked_.push_back("device:" + peer_id);
      if (!device_card_ || device_card_->keys.account_id != account_id) {
        done(Error("not reached"));
        return;
      }
      done(keys_->AddAccountCard(*device_card_, device_card_->issued_at, "device"));
    });
    workflow_ = std::make_unique<ContactRefreshWorkflow>(*contacts_, *keys_, directory_, *exchange_);
    workflow_->SetPosts([this](std::function<void()> task) { blocking_.push_back(std::move(task)); },
                        [this](std::function<void()> task) { ui_.push_back(std::move(task)); });
  }
  void TearDown() override {
    workflow_.reset();
    exchange_.reset();
    keys_.reset();
    cards_.reset();
    contacts_.reset();
    std::filesystem::remove_all(dir_);
  }

  static void RunAll(std::vector<std::function<void()>>& lane) {
    auto tasks = std::move(lane);
    lane.clear();
    for (auto& task : tasks) {
      task();
    }
  }

  /** Starts a refresh and runs both lanes to the end: the result. */
  ContactRefreshResult Refresh(const std::string& contact_id) {
    std::optional<ContactRefreshResult> result;
    EXPECT_TRUE(workflow_->Start(contact_id, [&](ContactRefreshResult r) { result = std::move(r); }));
    RunAll(blocking_);
    RunAll(ui_);
    EXPECT_TRUE(result.has_value());
    return result.value_or(ContactRefreshResult{});
  }

  AccountCard SignedCard(const test::TestAccount& account, const std::string& nickname,
                         std::vector<AccountCardDevice> devices, int64_t issued_at) {
    AccountCard card{.keys = account.Card(), .nickname = nickname, .devices = std::move(devices), .issued_at = issued_at};
    return SignAccountCard(card, [&account](const ByteVector& bytes) { return account.Sign(bytes); }).value();
  }

  /** Bob as a QR code adds him: his Account ID and a device, no relay id. */
  std::string AddBobByQr() {
    mesh::DirectoryHit hit;
    hit.hit_id = bob_.account_id;
    hit.account_id = bob_.account_id;
    hit.nickname = "bob";
    hit.ids = {{mesh::ContactIdKind::Account, bob_.account_id, true}};
    hit.endpoints = {mesh::DirectoryEndpoint{.peer_id = test::PeerIdOf("QmBobPhone")}};
    auto added = contacts_->AddFromDirectoryHit(hit);
    EXPECT_TRUE(added) << added.error().message;
    return added ? added->id : std::string{};
  }

  /** Our directory's entry for Bob, its keys his own. */
  mesh::DirectoryHit DirectoryBob() {
    mesh::DirectoryHit hit;
    hit.hit_id = "relay:bob";
    hit.account_id = bob_.account_id;
    hit.nickname = "Bob D.";
    hit.ids = {{mesh::ContactIdKind::Account, bob_.account_id, true},
               {mesh::ContactIdKind::RelayUser, "relay:bob", false}};
    hit.signing_public_key_b64 = bob_.SigningPublicB64();
    hit.kem_public_key_b64 = bob_.KemPublicB64();
    hit.kem_binding_signature_b64 = bob_.KemBindingB64();
    hit.icon = mesh::ProfileIconRef{"https://cdn.example/avatars/bob.png", "bob-photo", ""};
    hit.endpoints = {mesh::DirectoryEndpoint{
        .peer_id = test::PeerIdOf("QmBobPhone"), .multiaddrs = {"/ip4/192.0.2.7/udp/4001/quic-v1"}, .updated_at = 5}};
    return hit;
  }

  Contact Stored(const std::string& contact_id) { return **contacts_->Get(contact_id); }
  bool HasKemKey(const std::string& account_id) {
    return kem_.Get(ContactIdKindToString(mesh::ContactIdKind::Account), account_id).has_value();
  }

  std::filesystem::path dir_;
  std::vector<std::string> asked_;
  RecordingDirectory directory_{asked_};
  PeerSigningKeyStore signing_;
  PeerKemKeyStore kem_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<InMemoryPeerCards> cards_;
  std::unique_ptr<AccountKeyResolver> keys_;
  std::unique_ptr<ContactCardExchange> exchange_;
  std::unique_ptr<ContactRefreshWorkflow> workflow_;
  std::optional<AccountCard> device_card_;
  std::vector<std::function<void()>> blocking_;
  std::vector<std::function<void()>> ui_;
  test::TestAccount bob_ = test::TestAccount::Make();
  test::TestAccount mallory_ = test::TestAccount::Make();
};

TEST_F(ContactRefreshWorkflowTest, AsksTheDevicesFirstThenTheDirectoryAndMergesBoth) {
  const std::string id = AddBobByQr();
  // A card is kept already: the refresh still asks the devices for a newer one.
  ASSERT_TRUE(keys_->AddAccountCard(SignedCard(bob_, "bob", test::Devices({"QmBobPhone"}), kNow), kNow, "device"));
  device_card_ = SignedCard(bob_, "Bob B.", test::Devices({"QmBobPhone", "QmBobLaptop"}), kNow + 1000);
  directory_.answer = DirectoryBob();

  const ContactRefreshResult result = Refresh(id);

  EXPECT_EQ(asked_, (std::vector<std::string>{"device:" + test::PeerIdOf("QmBobPhone"),
                                              "directory:account:" + bob_.account_id}))
      << "the devices first, then our directory by Account ID";
  EXPECT_EQ(result.devices, ContactRefreshResult::Devices::Answered);
  EXPECT_EQ(result.directory, ContactRefreshResult::Directory::Answered);
  EXPECT_TRUE(result.name_changed);
  EXPECT_TRUE(result.photo_changed);
  EXPECT_TRUE(result.devices_changed);
  EXPECT_FALSE(result.keys_changed) << "same keys, a newer card";
  EXPECT_FALSE(result.directory_keys_rejected);

  const Contact stored = Stored(id);
  EXPECT_EQ(stored.remote.nickname, "Bob B.") << "a card fetched now from a device names the account";
  ASSERT_TRUE(stored.remote.icon.has_value());
  EXPECT_EQ(stored.remote.icon->url, "https://cdn.example/avatars/bob.png") << "the photo is the directory's";
  bool laptop = false;
  bool phone_addressed = false;
  for (const mesh::DirectoryEndpoint& row : stored.remote.endpoints) {
    laptop = laptop || row.peer_id == test::PeerIdOf("QmBobLaptop");
    phone_addressed = phone_addressed || (row.peer_id == test::PeerIdOf("QmBobPhone") && !row.multiaddrs.empty());
  }
  EXPECT_TRUE(laptop) << "the card's new device is a device row";
  EXPECT_TRUE(phone_addressed) << "the directory's addresses for a known device";
  EXPECT_GT(stored.remote.fetched_at, 0);
  ASSERT_TRUE(result.contact.has_value());
  EXPECT_EQ(result.contact->remote.nickname, "Bob B.");
}

TEST_F(ContactRefreshWorkflowTest, WithNoDeviceReachedTheDirectoryNamesThem) {
  const std::string id = AddBobByQr();
  directory_.answer = DirectoryBob();

  const ContactRefreshResult result = Refresh(id);

  EXPECT_EQ(result.devices, ContactRefreshResult::Devices::NotReached);
  EXPECT_EQ(Stored(id).remote.nickname, "Bob D.");
  EXPECT_TRUE(result.keys_changed) << "the directory's keys verified against his Account ID";
  EXPECT_TRUE(HasKemKey(bob_.account_id));
}

TEST_F(ContactRefreshWorkflowTest, DirectoryKeysThatDoNotMatchTheAccountIdAreNotKept) {
  const std::string id = AddBobByQr();
  mesh::DirectoryHit forged = DirectoryBob();
  // Mallory's keys under Bob's Account ID: they do not hash to it.
  forged.signing_public_key_b64 = mallory_.SigningPublicB64();
  forged.kem_public_key_b64 = mallory_.KemPublicB64();
  forged.kem_binding_signature_b64 = mallory_.KemBindingB64();
  directory_.answer = forged;

  const ContactRefreshResult result = Refresh(id);

  EXPECT_TRUE(result.directory_keys_rejected);
  EXPECT_FALSE(result.keys_changed);
  EXPECT_FALSE(HasKemKey(bob_.account_id)) << "no key kept for Bob";
  EXPECT_FALSE(keys_->Kept(bob_.account_id).value().has_value());
  EXPECT_EQ(Stored(id).remote.nickname, "Bob D.") << "ignored for keys only: the name and photo still count";
}

TEST_F(ContactRefreshWorkflowTest, AKemKeyTheAccountDidNotBindIsNotKept) {
  const std::string id = AddBobByQr();
  mesh::DirectoryHit swapped = DirectoryBob();
  swapped.kem_public_key_b64 = mallory_.KemPublicB64(); // Bob's signing key, someone else's KEM key
  directory_.answer = swapped;

  const ContactRefreshResult result = Refresh(id);

  EXPECT_TRUE(result.directory_keys_rejected);
  EXPECT_FALSE(HasKemKey(bob_.account_id));
}

TEST_F(ContactRefreshWorkflowTest, AnAnswerForAnotherAccountIsNotUsedAtAll) {
  const std::string id = AddBobByQr();
  mesh::DirectoryHit other = DirectoryBob();
  other.account_id = mallory_.account_id;
  other.ids = {{mesh::ContactIdKind::Account, mallory_.account_id, true}};
  other.signing_public_key_b64 = mallory_.SigningPublicB64();
  other.kem_public_key_b64 = mallory_.KemPublicB64();
  other.kem_binding_signature_b64 = mallory_.KemBindingB64();
  directory_.answer = other;

  const ContactRefreshResult result = Refresh(id);

  EXPECT_TRUE(result.directory_keys_rejected);
  EXPECT_FALSE(result.Changed());
  EXPECT_EQ(Stored(id).remote.nickname, "bob");
  EXPECT_FALSE(HasKemKey(mallory_.account_id)) << "keys for an account we did not ask about are not kept";
}

TEST_F(ContactRefreshWorkflowTest, TheLookupRunsOnTheBlockingLaneAndTheResultOnTheUiLane) {
  const std::string id = AddBobByQr();
  directory_.answer = DirectoryBob();
  std::optional<ContactRefreshResult> result;
  ASSERT_TRUE(workflow_->Start(id, [&](ContactRefreshResult r) { result = std::move(r); }));

  EXPECT_EQ(asked_, (std::vector<std::string>{"device:" + test::PeerIdOf("QmBobPhone")}))
      << "nothing blocking on the caller's thread";
  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(workflow_->Running(id));
  EXPECT_FALSE(workflow_->Start(id, [](ContactRefreshResult) { ADD_FAILURE() << "a second press runs nothing"; }));

  RunAll(blocking_);
  EXPECT_EQ(asked_.size(), 2u) << "the directory is asked on the blocking lane";
  EXPECT_FALSE(result.has_value()) << "the result waits for the UI lane";
  EXPECT_EQ(Stored(id).remote.nickname, "bob") << "the contact is written on the UI lane too";

  RunAll(ui_);
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(workflow_->Running(id));
  EXPECT_EQ(Stored(id).remote.nickname, "Bob D.");
}

TEST_F(ContactRefreshWorkflowTest, AContactWithoutARelayIdRefreshesByAccountId) {
  const std::string id = AddBobByQr();
  ASSERT_FALSE(ContactRelayUserId(Stored(id)).has_value());
  directory_.answer = DirectoryBob();

  const ContactRefreshResult result = Refresh(id);

  for (const std::string& asked : asked_) {
    EXPECT_EQ(asked.find("directory:relay:"), std::string::npos) << asked;
  }
  EXPECT_EQ(result.directory, ContactRefreshResult::Directory::Answered);
  EXPECT_TRUE(result.Changed());
  EXPECT_TRUE(HasKemKey(bob_.account_id)) << "messageable now";
  EXPECT_EQ(ContactRelayUserId(Stored(id)).value_or(""), "relay:bob") << "our relay's route to the account";
}

TEST_F(ContactRefreshWorkflowTest, AContactKnownOnlyByRelayIdIsLookedUpByItAndTakesAVerifiedAccount) {
  // An older contact: a relay id and no Account ID (a directory add no longer makes one).
  auto empty = contacts_->AddEmpty();
  ASSERT_TRUE(empty);
  Contact relay_only = *empty;
  relay_only.remote.nickname = "bob";
  relay_only.remote.ids = {{mesh::ContactIdKind::RelayUser, "relay:bob", true}};
  SyncContactMirrors(relay_only);
  auto added = contacts_->Upsert(relay_only);
  ASSERT_TRUE(added) << added.error().message;
  directory_.answer = DirectoryBob();

  const ContactRefreshResult result = Refresh(added->id);

  EXPECT_EQ(asked_, (std::vector<std::string>{"directory:relay:relay:bob"})) << "no Account ID: no device to ask";
  EXPECT_EQ(result.devices, ContactRefreshResult::Devices::NotAsked);
  EXPECT_EQ(ContactAccountId(Stored(added->id)).value_or(""), bob_.account_id);
}

TEST_F(ContactRefreshWorkflowTest, NothingAnsweringSaysWhichSourceFailed) {
  const std::string id = AddBobByQr();
  directory_.answer = Error("GET /v1/users/by-account failed with status 503");

  const ContactRefreshResult unreachable = Refresh(id);
  EXPECT_EQ(unreachable.devices, ContactRefreshResult::Devices::NotReached);
  EXPECT_EQ(unreachable.directory, ContactRefreshResult::Directory::Unreachable);
  EXPECT_TRUE(unreachable.NothingAnswered());
  EXPECT_FALSE(unreachable.Changed());

  directory_.answer = Error("Account not found");
  const ContactRefreshResult not_found = Refresh(id);
  EXPECT_EQ(not_found.directory, ContactRefreshResult::Directory::NotFound);
}

TEST(MergeContactRefreshTest, TheUsersOwnFieldsAreNeverTouched) {
  Contact contact;
  contact.id = "c1";
  contact.local.display_name = "My Bob";
  contact.local.trust = TrustLevel::Friendly;
  contact.remote.nickname = "bob";
  contact.remote.ids = {{mesh::ContactIdKind::Account, "account:bob", true}};
  mesh::DirectoryHit hit;
  hit.account_id = "account:bob";
  hit.nickname = "Bob D.";
  ContactRefreshResult result;
  const Contact merged = MergeContactRefresh(contact, ContactRefreshInputs{.hit = hit}, kNow, result);
  EXPECT_EQ(merged.local.display_name, "My Bob");
  EXPECT_EQ(merged.local.trust, TrustLevel::Friendly);
  EXPECT_EQ(merged.remote.nickname, "Bob D.");
  EXPECT_TRUE(result.name_changed);
}

} // namespace
} // namespace pbr
