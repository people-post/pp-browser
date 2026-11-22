#include "feature/conversations/ContactRefreshWorkflow.h"

#include "common/directory/DirectoryJson.h"

#include "lib/messaging/tests/TestAccount.h"
#include "lib/people/AccountCard.h"
#include "lib/people/ContactIdentity.h"
#include "lib/people/testing/InMemoryPeerCards.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/MeshAlias.h"

namespace pbr {
namespace {

constexpr int64_t kNow = 1'761'235'200'000;
constexpr const char kBobsA[] = "https://a.example/api/relay";
constexpr const char kBobsB[] = "https://b.example/api/relay";
constexpr const char kOurs[] = "https://ours.example/api/relay";
constexpr const char kOurOther[] = "https://ours-too.example/api/relay";

using Asked = std::vector<std::string>;
using Ask = ContactRefreshResult::RelayAsk;

/** No directory: the key resolver never looks anyone up here (the workflow asks relays itself). */
class NoDirectory : public mesh::IDirectoryClient {
public:
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string&) override { return Error("none"); }
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string&) override { return Error("none"); }
};

Error Down() {
  return Error("connect failed");
}

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
    workflow_ = std::make_unique<ContactRefreshWorkflow>(*contacts_, *keys_, *exchange_);
    workflow_->SetPosts([this](std::function<void()> task) { blocking_.push_back(std::move(task)); },
                        [this](std::function<void()> task) { ui_.push_back(std::move(task)); });
    // A relay answers as the test scripted it; one not scripted does not know the account.
    workflow_->SetLookup([this](const std::string& origin, const std::string& account_id,
                                const bool public_only) -> Roe<std::optional<StoredAccountRow>> {
      asked_.push_back("relay:" + origin);
      public_only_[origin] = public_only;
      EXPECT_EQ(account_id, bob_.account_id) << "relays are asked by Account ID";
      auto it = relays_.find(origin);
      return it == relays_.end() ? Roe<std::optional<StoredAccountRow>>(std::optional<StoredAccountRow>{}) : it->second;
    });
    workflow_->SetOurRelays({kOurs, kOurOther});
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
    asked_.clear();
    std::optional<ContactRefreshResult> result;
    EXPECT_TRUE(workflow_->Start(contact_id, [&](ContactRefreshResult r) { result = std::move(r); }));
    RunAll(blocking_);
    RunAll(ui_);
    EXPECT_TRUE(result.has_value());
    return result.value_or(ContactRefreshResult{});
  }

  AccountCard SignedCard(const test::TestAccount& account, const std::string& nickname,
                         std::vector<AccountCardDevice> devices, int64_t issued_at,
                         std::vector<std::string> relays = {}) {
    AccountCard card{
        .keys = account.Card(), .nickname = nickname, .devices = std::move(devices), .issued_at = issued_at};
    uint8_t fill = 0x50;
    for (std::string& origin : relays) {
      card.relays.push_back(test::CardRelay(origin, fill++));
    }
    return SignAccountCard(card, [&account](const ByteVector& bytes) { return account.Sign(bytes); }).value();
  }

  /** Bob's card kept, listing `relays` (preferred first). */
  void KeepBobsCard(std::vector<std::string> relays) {
    ASSERT_TRUE(keys_->AddAccountCard(SignedCard(bob_, "bob", test::Devices({"QmBobPhone"}), kNow, std::move(relays)),
                                      kNow, "device"));
  }

  /** Bob as a QR code adds him: his Account ID and a device (and the locator's relay, if any). */
  std::string AddBobByQr(const std::string& locator_relay = {}) {
    mesh::DirectoryHit hit;
    hit.hit_id = bob_.account_id;
    hit.account_id = bob_.account_id;
    hit.nickname = "bob";
    hit.ids = {{mesh::ContactIdKind::Account, bob_.account_id, true}};
    hit.endpoints = {mesh::DirectoryEndpoint{.peer_id = test::PeerIdOf("QmBobPhone")}};
    auto added = contacts_->AddFromDirectoryHit(hit);
    EXPECT_TRUE(added) << added.error().message;
    if (!added) {
      return {};
    }
    Contact contact = *added;
    contact.remote.locator_relay = locator_relay;
    EXPECT_TRUE(contacts_->Upsert(contact));
    return added->id;
  }

  /** A relay's row for Bob, its keys his own (and, given one, the card he registered there). */
  StoredAccountRow RelayBob(std::optional<AccountCard> card = std::nullopt) {
    mesh::DirectoryHit hit;
    hit.hit_id = bob_.account_id;
    hit.account_id = bob_.account_id;
    hit.nickname = "Bob D.";
    hit.ids = {{mesh::ContactIdKind::Account, bob_.account_id, true}};
    hit.signing_public_key_b64 = bob_.TypedSigningPublicB64();
    hit.kem_public_key_b64 = bob_.TypedKemPublicB64();
    hit.kem_binding_b64 = bob_.KemBindingB64();
    hit.icon = mesh::ProfileIconRef{"https://cdn.example/avatars/bob.png", "bob-photo", ""};
    hit.endpoints = {mesh::DirectoryEndpoint{
        .peer_id = test::PeerIdOf("QmBobPhone"), .multiaddrs = {"/ip4/192.0.2.7/udp/4001/quic-v1"}, .updated_at = 5}};
    StoredAccountRow row{.hit = std::move(hit)};
    if (card) {
      row.card = AccountCardToJson(*card);
    }
    return row;
  }
  void Answers(const std::string& origin, StoredAccountRow row) {
    relays_.insert_or_assign(origin,
                             Roe<std::optional<StoredAccountRow>>(std::optional<StoredAccountRow>(std::move(row))));
  }
  void IsDown(const std::string& origin) {
    relays_.insert_or_assign(origin, Roe<std::optional<StoredAccountRow>>(Down()));
  }

  Contact Stored(const std::string& contact_id) { return **contacts_->Get(contact_id); }
  ContactRelayView PageRelay(const std::string& contact_id) { return workflow_->PreferredRelay(Stored(contact_id)); }
  bool HasKemKey(const std::string& account_id) {
    return kem_.Get(ContactIdKindToString(mesh::ContactIdKind::Account), account_id).has_value();
  }

  std::filesystem::path dir_;
  Asked asked_;
  /** Per relay asked: whether it was asked over a public address only. */
  std::map<std::string, bool> public_only_;
  std::map<std::string, Roe<std::optional<StoredAccountRow>>> relays_;
  NoDirectory directory_;
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

// DESIGN § App, the refresh button: the contact's devices, then its relays in list order, then our relays.
TEST_F(ContactRefreshWorkflowTest, AsksTheDevicesThenTheirRelaysInOrderThenOurs) {
  const std::string id = AddBobByQr();
  KeepBobsCard({kBobsA, kBobsB, kOurs});
  IsDown(kBobsA);
  // kBobsB does not know him (not scripted); kOurs is one of his and of ours: asked once, as his.
  Answers(kOurOther, RelayBob());

  const ContactRefreshResult result = Refresh(id);

  EXPECT_EQ(asked_,
            (Asked{"device:" + test::PeerIdOf("QmBobPhone"), std::string("relay:") + kBobsA,
                   std::string("relay:") + kBobsB, std::string("relay:") + kOurs, std::string("relay:") + kOurOther}));
  EXPECT_EQ(result.relays, (std::vector<Ask>{{kBobsA, Ask::Whose::Theirs, Ask::Outcome::Unreachable},
                                             {kBobsB, Ask::Whose::Theirs, Ask::Outcome::NotFound},
                                             {kOurs, Ask::Whose::Theirs, Ask::Outcome::NotFound},
                                             {kOurOther, Ask::Whose::Ours, Ask::Outcome::Answered}}));
  ASSERT_NE(result.AnsweredBy(), nullptr);
  EXPECT_EQ(result.AnsweredBy()->origin, kOurOther);
  EXPECT_EQ(Stored(id).remote.nickname, "Bob D.");
  // His relays came from his card: asked over https to public addresses only (the lab's CA bundle is not set here).
  EXPECT_EQ(public_only_,
            (std::map<std::string, bool>{{kBobsA, true}, {kBobsB, true}, {kOurs, true}, {kOurOther, false}}));
}

TEST_F(ContactRefreshWorkflowTest, ARelayDownMeansTheNextOneAndTheFirstThatKnowsThemEndsIt) {
  const std::string id = AddBobByQr();
  KeepBobsCard({kBobsA, kBobsB});
  IsDown(kBobsA);
  Answers(kBobsB, RelayBob());

  const ContactRefreshResult result = Refresh(id);

  EXPECT_EQ(asked_, (Asked{"device:" + test::PeerIdOf("QmBobPhone"), std::string("relay:") + kBobsA,
                           std::string("relay:") + kBobsB}))
      << "our relays are not asked once one of his answered";
  ASSERT_NE(result.AnsweredBy(), nullptr);
  EXPECT_EQ(result.AnsweredBy()->whose, Ask::Whose::Theirs);
  EXPECT_TRUE(result.photo_changed);
  EXPECT_EQ(Stored(id).remote.icon->url, "https://cdn.example/avatars/bob.png") << "the photo is the relay's";
}

TEST_F(ContactRefreshWorkflowTest, AContactWithNoRelaysAsksOursOnly) {
  const std::string id = AddBobByQr(); // no card, no locator relay
  Answers(kOurs, RelayBob());

  const ContactRefreshResult result = Refresh(id);

  EXPECT_EQ(asked_, (Asked{"device:" + test::PeerIdOf("QmBobPhone"), std::string("relay:") + kOurs}));
  EXPECT_EQ(result.relays, (std::vector<Ask>{{kOurs, Ask::Whose::Ours, Ask::Outcome::Answered}}));
  EXPECT_TRUE(result.keys_changed) << "the relay's keys verified against his Account ID";
  EXPECT_TRUE(HasKemKey(bob_.account_id));
}

TEST_F(ContactRefreshWorkflowTest, AContactAddedByQrAsksTheLocatorsRelayFirst) {
  const std::string id = AddBobByQr(kBobsB);
  Answers(kBobsB, RelayBob());

  const ContactRefreshResult result = Refresh(id);

  EXPECT_EQ(asked_, (Asked{"device:" + test::PeerIdOf("QmBobPhone"), std::string("relay:") + kBobsB}));
  EXPECT_EQ(result.relays, (std::vector<Ask>{{kBobsB, Ask::Whose::Theirs, Ask::Outcome::Answered}}));
}

TEST_F(ContactRefreshWorkflowTest, TheDevicesCardAndTheRelaysRowMerge) {
  const std::string id = AddBobByQr();
  KeepBobsCard({kBobsA});
  // A device has a newer card: the newest name, a second device.
  device_card_ = SignedCard(bob_, "Bob B.", test::Devices({"QmBobPhone", "QmBobLaptop"}), kNow + 1000, {kBobsA});
  Answers(kBobsA, RelayBob());

  const ContactRefreshResult result = Refresh(id);

  EXPECT_EQ(result.devices, ContactRefreshResult::Devices::Answered);
  EXPECT_TRUE(result.name_changed);
  EXPECT_TRUE(result.photo_changed);
  EXPECT_TRUE(result.devices_changed);
  EXPECT_FALSE(result.keys_changed) << "same keys, a newer card";
  EXPECT_FALSE(result.relay_keys_rejected);
  const Contact stored = Stored(id);
  EXPECT_EQ(stored.remote.nickname, "Bob B.") << "the newest card names the account, not the relay's row";
  bool laptop = false;
  bool phone_addressed = false;
  for (const mesh::DirectoryEndpoint& row : stored.remote.endpoints) {
    laptop = laptop || row.peer_id == test::PeerIdOf("QmBobLaptop");
    phone_addressed = phone_addressed || (row.peer_id == test::PeerIdOf("QmBobPhone") && !row.multiaddrs.empty());
  }
  EXPECT_TRUE(laptop) << "the card's new device is a device row";
  EXPECT_TRUE(phone_addressed) << "the relay's addresses for a known device";
  EXPECT_GT(stored.remote.fetched_at, 0);
  ASSERT_TRUE(result.contact.has_value());
  EXPECT_EQ(result.contact->remote.nickname, "Bob B.");
}

TEST_F(ContactRefreshWorkflowTest, ARelaysStoredCardIsKeptAndBringsItsRelayList) {
  const std::string id = AddBobByQr();
  KeepBobsCard({kBobsA});
  Answers(kBobsA, RelayBob(SignedCard(bob_, "Bob R.", test::Devices({"QmBobPhone"}), kNow + 2000, {kBobsB, kBobsA})));

  const ContactRefreshResult result = Refresh(id);

  EXPECT_FALSE(result.relay_keys_rejected);
  EXPECT_EQ(Stored(id).remote.nickname, "Bob R.") << "the newer signed card's name";
  EXPECT_EQ(PageRelay(id).origin, kBobsB) << "his newest list's preferred relay";
}

// A row whose keys this build cannot read (mesh v10's `keys_update_needed`): update needed, not a forgery. Nothing of
// it is used and the next relay is asked.
TEST_F(ContactRefreshWorkflowTest, RelayKeysThatNeedANewerAppAreUpdateNeededNotRejected) {
  const std::string id = AddBobByQr();
  StoredAccountRow newer = RelayBob();
  newer.hit.keys_update_needed = true;
  newer.hit.nickname = "Bob N.";
  Answers(kOurs, newer);

  const ContactRefreshResult result = Refresh(id);
  EXPECT_TRUE(result.relay_keys_update_needed);
  EXPECT_FALSE(result.relay_keys_rejected);
  EXPECT_EQ(result.relays, (std::vector<Ask>{{kOurs, Ask::Whose::Ours, Ask::Outcome::Rejected},
                                             {kOurOther, Ask::Whose::Ours, Ask::Outcome::NotFound}}));
  EXPECT_FALSE(HasKemKey(bob_.account_id));
  EXPECT_EQ(Stored(id).remote.nickname, "bob") << "nothing of the row is used";
}

TEST_F(ContactRefreshWorkflowTest, RelayKeysThatDoNotMatchTheAccountIdContributeNothingAndTheNextRelayIsAsked) {
  const std::string id = AddBobByQr();
  StoredAccountRow forged = RelayBob();
  // Mallory's keys under Bob's Account ID: they do not hash to it. The rest of the row is hers too.
  forged.hit.signing_public_key_b64 = mallory_.TypedSigningPublicB64();
  forged.hit.kem_public_key_b64 = mallory_.TypedKemPublicB64();
  forged.hit.kem_binding_b64 = mallory_.KemBindingB64();
  forged.hit.nickname = "Mallory";
  forged.hit.icon = mesh::ProfileIconRef{"https://evil.example/mallory.png", "mallory-photo", ""};
  Answers(kOurs, forged);

  const ContactRefreshResult alone = Refresh(id);
  EXPECT_TRUE(alone.relay_keys_rejected);
  EXPECT_EQ(alone.relays, (std::vector<Ask>{{kOurs, Ask::Whose::Ours, Ask::Outcome::Rejected},
                                            {kOurOther, Ask::Whose::Ours, Ask::Outcome::NotFound}}))
      << "a rejected answer is not an answer: the walk goes on";
  EXPECT_TRUE(alone.NothingAnswered());
  EXPECT_FALSE(alone.Changed());
  EXPECT_FALSE(HasKemKey(bob_.account_id)) << "no key kept for Bob";
  EXPECT_FALSE(keys_->Kept(bob_.account_id).value().has_value());
  EXPECT_EQ(Stored(id).remote.nickname, "bob") << "not the forged row's name";
  EXPECT_FALSE(Stored(id).remote.icon.has_value() && !Stored(id).remote.icon->empty()) << "nor its photo";

  Answers(kOurOther, RelayBob());
  const ContactRefreshResult then = Refresh(id);
  ASSERT_NE(then.AnsweredBy(), nullptr);
  EXPECT_EQ(then.AnsweredBy()->origin, kOurOther) << "the next relay's verified answer is the one used";
  EXPECT_EQ(Stored(id).remote.nickname, "Bob D.");
  EXPECT_EQ(Stored(id).remote.icon->url, "https://cdn.example/avatars/bob.png");
}

TEST_F(ContactRefreshWorkflowTest, TheirRelayAnsweringUnverifiablyShowsAsNotReachable) {
  const std::string id = AddBobByQr();
  KeepBobsCard({kBobsA});
  StoredAccountRow forged = RelayBob();
  forged.hit.signing_public_key_b64 = mallory_.TypedSigningPublicB64();
  Answers(kBobsA, forged);

  const ContactRefreshResult result = Refresh(id);

  EXPECT_EQ(result.relays.front(), (Ask{kBobsA, Ask::Whose::Theirs, Ask::Outcome::Rejected}));
  EXPECT_EQ(PageRelay(id).reach, ContactRelayView::Reach::Unreachable) << "not Reachable on an answer that lies";
}

TEST_F(ContactRefreshWorkflowTest, ARelaysCardThatDoesNotVerifyIsNotKept) {
  const std::string id = AddBobByQr();
  KeepBobsCard({kBobsA});
  AccountCard forged = SignedCard(bob_, "Bob R.", test::Devices({"QmBobPhone"}), kNow + 2000, {kBobsB});
  forged.relays = {test::CardRelay("https://evil.example/api/relay", 0x66)}; // not what Bob signed
  Answers(kBobsA, RelayBob(forged));

  const ContactRefreshResult result = Refresh(id);

  EXPECT_TRUE(result.relay_keys_rejected);
  EXPECT_EQ(result.relays.front().outcome, Ask::Outcome::Rejected);
  EXPECT_EQ(PageRelay(id).origin, kBobsA) << "his list stays the one he signed";
  EXPECT_EQ(Stored(id).remote.nickname, "bob") << "an answer with a forged card contributes nothing";
}

TEST_F(ContactRefreshWorkflowTest, AKemKeyTheAccountDidNotBindIsNotKept) {
  const std::string id = AddBobByQr();
  StoredAccountRow swapped = RelayBob();
  swapped.hit.kem_public_key_b64 = mallory_.TypedKemPublicB64(); // Bob's signing key, someone else's KEM key
  Answers(kOurs, swapped);

  const ContactRefreshResult result = Refresh(id);

  EXPECT_TRUE(result.relay_keys_rejected);
  EXPECT_FALSE(HasKemKey(bob_.account_id));
}

TEST_F(ContactRefreshWorkflowTest, AnAnswerForAnotherAccountIsNotUsedAtAll) {
  const std::string id = AddBobByQr();
  StoredAccountRow other = RelayBob();
  other.hit.account_id = mallory_.account_id;
  other.hit.ids = {{mesh::ContactIdKind::Account, mallory_.account_id, true}};
  other.hit.signing_public_key_b64 = mallory_.TypedSigningPublicB64();
  other.hit.kem_public_key_b64 = mallory_.TypedKemPublicB64();
  other.hit.kem_binding_b64 = mallory_.KemBindingB64();
  Answers(kOurs, other);

  const ContactRefreshResult result = Refresh(id);

  EXPECT_TRUE(result.relay_keys_rejected);
  EXPECT_FALSE(result.Changed());
  EXPECT_EQ(result.relays.front(), (Ask{kOurs, Ask::Whose::Ours, Ask::Outcome::Rejected}));
  EXPECT_EQ(result.relays.size(), 2u) << "our other relay is asked next";
  EXPECT_EQ(Stored(id).remote.nickname, "bob");
  EXPECT_FALSE(HasKemKey(mallory_.account_id)) << "keys for an account we did not ask about are not kept";
}

TEST_F(ContactRefreshWorkflowTest, TheLookupsRunOnTheBlockingLaneAndTheResultOnTheUiLane) {
  const std::string id = AddBobByQr();
  Answers(kOurs, RelayBob());
  std::optional<ContactRefreshResult> result;
  ASSERT_TRUE(workflow_->Start(id, [&](ContactRefreshResult r) { result = std::move(r); }));

  EXPECT_EQ(asked_, (Asked{"device:" + test::PeerIdOf("QmBobPhone")})) << "nothing blocking on the caller's thread";
  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(workflow_->Running(id));
  EXPECT_FALSE(workflow_->Start(id, [](ContactRefreshResult) { ADD_FAILURE() << "a second press runs nothing"; }));

  RunAll(blocking_);
  EXPECT_EQ(asked_.size(), 2u) << "the relay is asked on the blocking lane";
  EXPECT_FALSE(result.has_value()) << "the result waits for the UI lane";
  EXPECT_EQ(Stored(id).remote.nickname, "bob") << "the contact is written on the UI lane too";

  RunAll(ui_);
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(workflow_->Running(id));
  EXPECT_EQ(Stored(id).remote.nickname, "Bob D.");
  EXPECT_EQ(ContactAccountId(Stored(id)).value_or(""), bob_.account_id);
}

// Multi-relay MR001: relays know people by Account ID only, so a contact without one (named by something else)
// asks neither its devices nor any relay.
TEST_F(ContactRefreshWorkflowTest, AContactWithoutAnAccountIdAsksNoOne) {
  auto empty = contacts_->AddEmpty();
  ASSERT_TRUE(empty);
  Contact other = *empty;
  other.remote.nickname = "bob";
  other.remote.ids = {{mesh::ContactIdKind::Custom, "custom:bob", true}};
  SyncContactMirrors(other);
  auto added = contacts_->Upsert(other);
  ASSERT_TRUE(added) << added.error().message;
  Answers(kOurs, RelayBob());

  const ContactRefreshResult result = Refresh(added->id);

  EXPECT_TRUE(asked_.empty());
  EXPECT_EQ(result.devices, ContactRefreshResult::Devices::NotAsked);
  EXPECT_TRUE(result.relays.empty());
  EXPECT_FALSE(ContactAccountId(Stored(added->id)).has_value());
}

TEST_F(ContactRefreshWorkflowTest, NothingAnsweringSaysWhichSourcesFailed) {
  const std::string id = AddBobByQr();
  IsDown(kOurs);
  IsDown(kOurOther);

  const ContactRefreshResult unreachable = Refresh(id);
  EXPECT_EQ(unreachable.devices, ContactRefreshResult::Devices::NotReached);
  EXPECT_EQ(unreachable.relays, (std::vector<Ask>{{kOurs, Ask::Whose::Ours, Ask::Outcome::Unreachable},
                                                  {kOurOther, Ask::Whose::Ours, Ask::Outcome::Unreachable}}));
  EXPECT_TRUE(unreachable.NothingAnswered());
  EXPECT_FALSE(unreachable.Changed());

  relays_.clear();
  const ContactRefreshResult not_found = Refresh(id);
  EXPECT_EQ(not_found.relays, (std::vector<Ask>{{kOurs, Ask::Whose::Ours, Ask::Outcome::NotFound},
                                                {kOurOther, Ask::Whose::Ours, Ask::Outcome::NotFound}}));
  EXPECT_TRUE(not_found.NothingAnswered());
}

// DESIGN § App: the contact page shows the preferred relay with its reachability, "not known" until a refresh asked.
TEST_F(ContactRefreshWorkflowTest, ThePreferredRelaysReachabilityIsWhatItLastAnswered) {
  const std::string id = AddBobByQr();
  KeepBobsCard({kBobsA, kBobsB});
  EXPECT_EQ(PageRelay(id),
            (ContactRelayView{
                .list = ContactRelayView::List::Listed, .origin = kBobsA, .reach = ContactRelayView::Reach::NotKnown}));

  IsDown(kBobsA);
  Answers(kBobsB, RelayBob());
  Refresh(id);
  EXPECT_EQ(PageRelay(id).reach, ContactRelayView::Reach::Unreachable)
      << "the preferred one, not the one that answered";

  Answers(kBobsA, RelayBob());
  Refresh(id);
  EXPECT_EQ(PageRelay(id).reach, ContactRelayView::Reach::Reachable);

  relays_.erase(kBobsA);
  Refresh(id);
  EXPECT_EQ(PageRelay(id).reach, ContactRelayView::Reach::NotRegistered);
}

// MR007 § Pins (what the user sees): the page says *identity doesn't match* when the relay id the contact's card names
// for its preferred relay disagrees with our pins (or a send found its audience refused) — whatever the relay last
// answered.
TEST_F(ContactRefreshWorkflowTest, APreferredRelayWhoseIdentityDoesNotMatchSaysSo) {
  const std::string id = AddBobByQr();
  KeepBobsCard({kBobsA, kBobsB});
  std::vector<std::string> asked;
  workflow_->SetIdentityMismatch(
      [&asked](const std::string& account, const std::string& origin, const RelayId& relay_id) {
        asked.push_back(account + " " + origin + " " + relay_id.ToString());
        return origin == kBobsA;
      });
  Answers(kBobsA, RelayBob());
  Refresh(id);
  EXPECT_EQ(PageRelay(id).reach, ContactRelayView::Reach::IdentityMismatch);
  ASSERT_FALSE(asked.empty());
  EXPECT_EQ(asked.back(), bob_.account_id + " " + kBobsA + " " + test::RelayIdOf(0x50).ToString())
      << "the card's relay id";
  workflow_->SetIdentityMismatch(nullptr);
  EXPECT_EQ(PageRelay(id).reach, ContactRelayView::Reach::Reachable);
}

TEST_F(ContactRefreshWorkflowTest, APageWithoutARelaySaysNoneListedOrNotKnown) {
  const std::string id = AddBobByQr();
  EXPECT_EQ(PageRelay(id).list, ContactRelayView::List::NotKnown) << "no card yet";
  const std::string by_qr = AddBobByQr(kBobsB);
  EXPECT_EQ(PageRelay(by_qr).origin, kBobsB) << "the locator's relay until the card";
  KeepBobsCard({});
  EXPECT_EQ(PageRelay(id).list, ContactRelayView::List::NoneListed) << "his card lists no relay";
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
  const Contact merged =
      MergeContactRefresh(contact, ContactRefreshInputs{.hit = hit, .hit_keys_verified = true}, kNow, result);
  EXPECT_EQ(merged.local.display_name, "My Bob");
  EXPECT_EQ(merged.local.trust, TrustLevel::Friendly);
  EXPECT_EQ(merged.remote.nickname, "Bob D.");
  EXPECT_TRUE(result.name_changed);
}

TEST(MergeContactRefreshTest, AnUnverifiedRelayRowContributesNothing) {
  Contact contact;
  contact.id = "c1";
  contact.remote.nickname = "bob";
  contact.remote.ids = {{mesh::ContactIdKind::Account, "account:bob", true}};
  mesh::DirectoryHit hit;
  hit.account_id = "account:bob";
  hit.nickname = "Mallory";
  hit.icon = mesh::ProfileIconRef{"https://evil.example/m.png", "m", ""};
  hit.endpoints = {mesh::DirectoryEndpoint{.peer_id = "QmMallory", .multiaddrs = {"/ip4/192.0.2.9/udp/1/quic-v1"}}};
  ContactRefreshResult result;
  const Contact merged = MergeContactRefresh(contact, ContactRefreshInputs{.hit = hit}, kNow, result);
  EXPECT_EQ(merged.remote.nickname, "bob");
  EXPECT_TRUE(merged.remote.endpoints.empty());
  EXPECT_FALSE(result.Changed());
}

} // namespace
} // namespace pbr
