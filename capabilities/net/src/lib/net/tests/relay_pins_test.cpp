#include "lib/net/RelayIdentity.h"
#include "lib/net/RelayPinStore.h"
#include "lib/net/RelayPins.h"

#include "common/ValueJson.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/error/AppError.h"

#include "crypto/PeerId.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace pbr {
namespace {

// MR008-PIN: a relay address carries its PeerId; the pins are the identities we expect, from addresses (a relay link,
// the build, the config, our own card on a linked device), never from the relay's own answer but where first use is
// allowed (a developer build's bare origin, the org's without a shipped PeerId); one PeerId per origin and one origin
// per PeerId; an identity answer naming another PeerId leaves the relay not used, with no way to accept it
// (docs/contracts/SERVICE_ENDPOINTS.md § Relay identity, § Pins; DATA_LAYOUT.md `relay_pins.json`).

constexpr const char kA[] = "https://a.example/api/relay";
constexpr const char kB[] = "https://b.example/api/relay";
constexpr const char kC[] = "https://c.example/api/relay";
constexpr const char kPeerA[] = "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP";
constexpr const char kPeerB[] = "QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie";
constexpr const char kPeerC[] = "QmbSD2vbEJTdL9PW5w5mr3pak9EYyoiYfiqCPJJ6AxJBig";

std::string IdentityBody(const ByteVector& key, int64_t schema = 1, const std::string& alg = "ml-dsa-65") {
  Object body;
  body.set("schema_version", schema);
  body.set("peer_id", pp::PeerIdFromMlDsaPublicKey(key).value());
  body.set("public_key_b64", Base64Encode(key));
  body.set("signature_alg", alg);
  body.set("something_new", true); // unknown keys are ignored
  return DumpJson(body);
}

TEST(RelayIdentityTest, AnAnswerIsTakenOnlyWhenTheKeyDerivesThePeerId) {
  const ByteVector key(kMlDsa65PublicKeyBytes, 0x42);
  auto identity = ParseRelayIdentity(IdentityBody(key));
  ASSERT_TRUE(identity) << identity.error().message;
  EXPECT_EQ(identity->peer_id, kPeerA);

  Object wrong = TryParseObject(IdentityBody(key)).value();
  wrong.set("peer_id", std::string(kPeerB));
  EXPECT_FALSE(ParseRelayIdentity(DumpJson(wrong))) << "a PeerId its key does not derive";
  EXPECT_FALSE(ParseRelayIdentity(IdentityBody(key, 2))) << "a newer schema";
  EXPECT_FALSE(ParseRelayIdentity(IdentityBody(key, 1, "ed25519")));
  Object short_key = TryParseObject(IdentityBody(key)).value();
  short_key.set("public_key_b64", Base64Encode(ByteVector(32, 0x42)));
  EXPECT_FALSE(ParseRelayIdentity(DumpJson(short_key))) << "not an ML-DSA-65 key";
  Object unpadded = TryParseObject(IdentityBody(key)).value();
  std::string b64 = Base64Encode(key);
  while (!b64.empty() && b64.back() == '=') {
    b64.pop_back();
  }
  unpadded.set("public_key_b64", b64);
  EXPECT_FALSE(ParseRelayIdentity(DumpJson(unpadded))) << "only canonical base64";
  EXPECT_FALSE(ParseRelayIdentity("not json"));
  const auto refused = ParseRelayIdentity("{}");
  ASSERT_FALSE(refused);
  EXPECT_EQ(static_cast<Err::Network>(refused.error().code), Err::Network::RelayIdentityUnavailable)
      << "handled as no identity";
}

class RelayPinsTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ =
        std::filesystem::temp_directory_path() / ("pp_relay_pins_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    Open();
  }
  void TearDown() override {
    pins_.reset();
    std::filesystem::remove_all(dir_);
  }
  void Open() {
    pins_ = std::make_shared<RelayPins>(dir_.string());
    pins_->SetClock([this]() { return now_ms_; });
    pins_->SetFetch([this](const std::string& origin, const bool public_only) -> Roe<RelayIdentity> {
      fetched_.push_back(origin);
      public_only_.push_back(public_only);
      const auto it = presents_.find(origin);
      if (it == presents_.end()) {
        return AppError::Network(Err::Network::RelayIdentityUnavailable, "relay_identity_unavailable: HTTP 503");
      }
      return RelayIdentity{.peer_id = it->second};
    });
    pins_->SetFirstUse([this](const std::string& origin) { return first_use_.contains(origin); });
    pins_->SetOnChanged([this](const std::string& origin) { changed_.push_back(origin); });
    pins_->SetOnNotRegistered([this](const std::string& origin) { lost_.push_back(origin); });
    pins_->SetOnUpgradeRequired([this](const std::string& origin) { too_old_.push_back(origin); });
  }

  std::filesystem::path dir_;
  int64_t now_ms_ = 1'761'235'200'000;
  std::map<std::string, std::string> presents_;
  std::set<std::string> first_use_;
  std::vector<std::string> fetched_;
  std::vector<bool> public_only_;
  std::vector<std::string> changed_;
  std::vector<std::string> lost_;
  std::vector<std::string> too_old_;
  std::shared_ptr<RelayPins> pins_;
};

TEST_F(RelayPinsTest, AnExpectedRelayIsSignedForTheAddressesPeerIdWithoutAskingIt) {
  ASSERT_TRUE(pins_->Expect(kA, kPeerA));
  auto audience = pins_->AudienceFor(kA);
  EXPECT_EQ(audience->PeerId().valueOr(""), kPeerA);
  EXPECT_TRUE(fetched_.empty()) << "the address names it: nothing is learned from the relay";
  EXPECT_EQ(changed_, std::vector<std::string>{kA});
  EXPECT_EQ(pins_->AudienceFor("HTTPS://A.example:443/api/relay/"), audience) << "one audience per relay";

  Open(); // restarted: the expectation is profile data
  EXPECT_EQ(pins_->PinnedPeerId(kA).value_or(""), kPeerA);
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), kPeerA);
  EXPECT_TRUE(fetched_.empty());
  EXPECT_FALSE(pins_->Expect(kA, "not-a-peer-id")) << "only a PeerId is expected";
}

// The review's attack (MR008 problem): without an expectation an origin's own answer is never taken (no first come).
TEST_F(RelayPinsTest, AnOriginNoAddressNamesIsNeverLearnedFromItsAnswer) {
  presents_[kA] = kPeerA;
  auto refused = pins_->AudienceFor(kA)->PeerId();
  ASSERT_FALSE(refused) << "nothing is signed for a relay whose address names no PeerId";
  EXPECT_EQ(static_cast<Err::Network>(refused.error().code), Err::Network::RelayIdentityUnavailable);
  EXPECT_FALSE(pins_->CheckIdentities({kA}));
  EXPECT_TRUE(fetched_.empty()) << "not even asked";
  EXPECT_FALSE(pins_->PinFor(kA));
}

TEST_F(RelayPinsTest, AnAnswerNamingAnotherPeerIdLeavesTheRelayUnusedAndIsNeverAccepted) {
  ASSERT_TRUE(pins_->Expect(kA, kPeerA));
  presents_[kA] = kPeerB; // the relay rotated its key, or another server sits at the origin
  changed_.clear();
  EXPECT_TRUE(pins_->CheckIdentities({kA}));
  EXPECT_TRUE(pins_->IdentityMismatch(kA));
  EXPECT_EQ(changed_, std::vector<std::string>{kA});
  auto refused = pins_->AudienceFor(kA)->PeerId();
  ASSERT_FALSE(refused) << "not used";
  EXPECT_EQ(static_cast<Err::Auth>(refused.error().code), Err::Auth::RelayIdentityMismatch);
  EXPECT_FALSE(pins_->PinnedPeerId(kA)) << "left off our card meanwhile";
  EXPECT_EQ(pins_->PinFor(kA)->peer_id, kPeerA) << "the address's PeerId stays: the answer is never taken";
  EXPECT_TRUE(pins_->Expect(kA, kPeerA)) << "the same address again changes nothing";
  EXPECT_TRUE(pins_->IdentityMismatch(kA));

  Open(); // the state survives a restart
  EXPECT_TRUE(pins_->IdentityMismatch(kA));
  presents_[kA] = kPeerA; // it answers to its address again (its operator undid the change)
  EXPECT_TRUE(pins_->CheckIdentities({kA}));
  EXPECT_FALSE(pins_->IdentityMismatch(kA));
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), kPeerA);

  presents_[kA] = kPeerB;
  pins_->AudienceFor(kA)->Refused("wrong_audience");
  ASSERT_TRUE(pins_->IdentityMismatch(kA)) << "a wrong_audience asks, and finds it so too";
  ASSERT_TRUE(pins_->Expect(kA, kPeerC)) << "a new key is a new address: the person adds its new link";
  EXPECT_FALSE(pins_->IdentityMismatch(kA));
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), kPeerC);
}

TEST_F(RelayPinsTest, EachRelayIsCheckedOncePerRunAndAgainWhileItDidNotAnswer) {
  ASSERT_TRUE(pins_->Expect(kA, kPeerA));
  ASSERT_TRUE(pins_->Expect(kB, kPeerB));
  presents_[kA] = kPeerA;
  EXPECT_FALSE(pins_->CheckIdentities({kA, kB})) << "both as expected or unanswered: nothing changed";
  EXPECT_EQ(fetched_, (std::vector<std::string>{kA, kB}));
  EXPECT_FALSE(pins_->CheckIdentities({kA, kB}));
  EXPECT_EQ(fetched_, (std::vector<std::string>{kA, kB, kB})) << "A checked already; B asked again";
  EXPECT_EQ(pins_->AudienceFor(kB)->PeerId().valueOr(""), kPeerB) << "unchecked is still signed for its address";
}

TEST_F(RelayPinsTest, WrongAudienceForTheExpectedPeerIdIsAMisbehavingRelayAndAskedAtMostEveryFiveMinutes) {
  ASSERT_TRUE(pins_->Expect(kA, kPeerA));
  presents_[kA] = kPeerA;
  auto audience = pins_->AudienceFor(kA);
  audience->Refused("wrong_audience");
  audience->Refused("wrong_audience");
  EXPECT_EQ(fetched_.size(), 1u) << "one identity fetch for the two refusals";
  EXPECT_FALSE(pins_->IdentityMismatch(kA));
  EXPECT_EQ(audience->PeerId().valueOr(""), kPeerA);
  now_ms_ += RelayPins::kWrongAudienceRefetchMs;
  audience->Refused("wrong_audience");
  EXPECT_EQ(fetched_.size(), 2u);
}

// MR008-UNIQ: a link (or a card) naming the PeerId of a relay we know at another address would front it: refused.
TEST_F(RelayPinsTest, APeerIdExpectedAtAnotherOriginIsRefused) {
  ASSERT_TRUE(pins_->Expect(kA, kPeerA)); // the org's relay, from the build
  auto taken = pins_->Expect(kB, kPeerA);
  ASSERT_FALSE(taken);
  EXPECT_EQ(static_cast<Err::Auth>(taken.error().code), Err::Auth::RelayIdentityTaken);
  EXPECT_FALSE(pins_->PinFor(kB));
  presents_[kB] = kPeerA; // a hostile relay at B that passes the org's answers through
  const AddIdentityCheck add = pins_->CheckForAdd(kB, kPeerA);
  EXPECT_EQ(add.result, AddIdentityCheck::Result::Taken);
  EXPECT_EQ(add.holder, "https://a.example/api/relay");
  EXPECT_TRUE(fetched_.empty()) << "refused before it is asked";
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), kPeerA) << "the org keeps its address";

  pins_->Drop(kA); // Remove… of a relay of ours: its PeerId is free again
  EXPECT_EQ(pins_->CheckForAdd(kB, kPeerA).result, AddIdentityCheck::Result::Ok);
}

// The review's attack, end to end: the org's PeerId ships with the build, so a relay that claims it first gains
// nothing, and the org's relay is never refused.
TEST_F(RelayPinsTest, AHostileRelayClaimingTheOrgsPeerIdFirstNeverBlocksTheOrg) {
  presents_[kB] = kPeerA; // the hostile relay claims the org's identity and is added (or scanned) first
  presents_[kA] = kPeerA;
  EXPECT_EQ(pins_->CheckForAdd(kB, kPeerB).result, AddIdentityCheck::Result::Mismatch)
      << "its link names its own PeerId, its answer another: not added";
  ASSERT_TRUE(pins_->Expect(kA, kPeerA)) << "the org's address is the build's: nothing stands against it";
  EXPECT_EQ(pins_->CheckForAdd(kB, kPeerA).result, AddIdentityCheck::Result::Taken);
  EXPECT_FALSE(pins_->CheckIdentities({kA}));
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), kPeerA);
}

TEST_F(RelayPinsTest, AddChecksTheAnswerAgainstTheLink) {
  presents_[kB] = kPeerC;
  const AddIdentityCheck mismatch = pins_->CheckForAdd(kB, kPeerB);
  EXPECT_EQ(mismatch.result, AddIdentityCheck::Result::Mismatch);
  EXPECT_FALSE(pins_->PinFor(kB)) << "nothing expected from an Add that did not go through";
  presents_[kB] = kPeerB;
  const AddIdentityCheck first = pins_->CheckForAdd(kB, kPeerB);
  EXPECT_EQ(first.result, AddIdentityCheck::Result::Ok);
  EXPECT_TRUE(first.pinned_now);
  EXPECT_EQ(first.peer_id, kPeerB);
  EXPECT_FALSE(pins_->CheckForAdd(kB, kPeerB).pinned_now) << "expected already";
  EXPECT_EQ(pins_->CheckForAdd(kB, kPeerC).result, AddIdentityCheck::Result::Mismatch)
      << "an origin expected to answer to another PeerId";
  presents_.erase(kA);
  EXPECT_EQ(pins_->CheckForAdd(kA, kPeerA).result, AddIdentityCheck::Result::Unavailable)
      << "a server that does not say which relay it is";
}

// MR008 item 4: a developer build's bare origin (and the org's in a build that ships no PeerId) on first use only.
TEST_F(RelayPinsTest, FirstUseOnlyWhereItIsAllowed) {
  presents_[kA] = kPeerA;
  presents_[kB] = kPeerB;
  first_use_.insert(kA);
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), kPeerA) << "pinned on its first answer";
  EXPECT_EQ(pins_->PinFor(kA)->peer_id, kPeerA);
  EXPECT_FALSE(pins_->AudienceFor(kB)->PeerId()) << "first use is not allowed for B";
  EXPECT_EQ(fetched_, std::vector<std::string>{kA});

  const AddIdentityCheck bare = pins_->CheckForAdd(kB, "");
  EXPECT_EQ(bare.result, AddIdentityCheck::Result::Ok) << "Add of a bare origin (developer builds only)";
  EXPECT_EQ(bare.peer_id, kPeerB);
  EXPECT_TRUE(bare.pinned_now);
  presents_[kC] = kPeerA;
  EXPECT_EQ(pins_->CheckForAdd(kC, "").result, AddIdentityCheck::Result::Taken);
}

TEST_F(RelayPinsTest, AFirstUseFetchThatFailsPinsNothingAndIsAskedAgainAfterAShortBackoff) {
  first_use_.insert(kA);
  auto audience = pins_->AudienceFor(kA);
  ASSERT_FALSE(audience->PeerId());
  EXPECT_FALSE(pins_->PinFor(kA));
  EXPECT_FALSE(audience->PeerId()) << "within the backoff: nothing signed, nothing asked";
  EXPECT_EQ(fetched_.size(), 1u);
  for (int i = 0; i < 6; ++i) {
    now_ms_ += RelayPins::kMaxOursRetryMs;
    ASSERT_FALSE(audience->PeerId());
  }
  EXPECT_EQ(fetched_.size(), 7u) << "asked again at least every kMaxOursRetryMs";
  presents_[kA] = kPeerA;
  EXPECT_FALSE(audience->PeerId()) << "within the backoff";
  EXPECT_TRUE(pins_->CheckIdentities({kA})) << "asked now, whatever the backoff";
  EXPECT_EQ(audience->PeerId().valueOr(""), kPeerA);
}

TEST_F(RelayPinsTest, AContactsCardWritesNoPinButIsCheckedAgainstThem) {
  ASSERT_TRUE(pins_->Expect(kA, kPeerA));
  EXPECT_EQ(pins_->CheckCardRelay(kA, kPeerA), CardRelayCheck::Usable);
  EXPECT_EQ(pins_->CheckCardRelay(kA, kPeerB), CardRelayCheck::Mismatch) << "we expect another PeerId there";
  EXPECT_EQ(pins_->CheckCardRelay(kB, kPeerA), CardRelayCheck::Mismatch) << "that PeerId is expected at another origin";
  EXPECT_EQ(pins_->CheckCardRelay(kB, kPeerC), CardRelayCheck::Usable) << "no pin for either: the card's word";
  EXPECT_FALSE(pins_->PinFor(kB)) << "checking writes nothing";
}

TEST_F(RelayPinsTest, OurOwnCardOnALinkedDeviceWritesPins) {
  ASSERT_TRUE(pins_->Expect(kA, kPeerA));
  // Our own account says B holds kPeerA now (the relay moved) and A holds kPeerC.
  pins_->TakeOwnCard({{.origin = kB, .peer_id = kPeerA}, {.origin = kA, .peer_id = kPeerC}});
  EXPECT_EQ(pins_->PinnedPeerId(kB).value_or(""), kPeerA);
  EXPECT_EQ(pins_->PinnedPeerId(kA).value_or(""), kPeerC);
  EXPECT_TRUE(fetched_.empty()) << "written from the card, not fetched";
}

TEST_F(RelayPinsTest, NotRegisteredGoesToTheOwnerAndWrongAudienceDoesNot) {
  ASSERT_TRUE(pins_->Expect(kA, kPeerA));
  auto audience = pins_->AudienceFor(kA);
  audience->Refused("not_registered");
  audience->Refused("registration_expired");
  audience->Refused("something_else");
  EXPECT_EQ(lost_, (std::vector<std::string>{kA, kA}));
  EXPECT_TRUE(too_old_.empty());
}

// 426 `client_upgrade_required`: this app is too old for that relay — told to the owner each time (the org's relay:
// the update screen; another of ours: its row), never taken for a lost registration or an identity question.
TEST_F(RelayPinsTest, UpgradeRequiredGoesToTheOwnerEachTime) {
  ASSERT_TRUE(pins_->Expect(kA, kPeerA));
  auto audience = pins_->AudienceFor(kA);
  audience->Refused("client_upgrade_required");
  audience->Refused("client_upgrade_required");
  EXPECT_EQ(too_old_, (std::vector<std::string>{kA, kA}));
  EXPECT_TRUE(lost_.empty());
  EXPECT_TRUE(fetched_.empty()) << "its identity is not asked again";
}

TEST_F(RelayPinsTest, OurCardsRelaysArePinnedOnesInForceInOrder) {
  ASSERT_TRUE(pins_->Expect(kA, kPeerA));
  const auto relays = pins_->PinnedRelays({kB, kA});
  ASSERT_EQ(relays.size(), 1u) << "B names no PeerId: left off";
  EXPECT_EQ(relays[0].origin, kA);
  EXPECT_EQ(relays[0].peer_id, kPeerA);
  ASSERT_TRUE(pins_->Expect(kB, kPeerB));
  EXPECT_EQ(pins_->PinnedRelays({kB, kA}).size(), 2u);
}

TEST(RelayPinStoreTest, TheBoundKeepsOursAndDropsTheOldestOther) {
  RelayPinStore store("");
  store.SetKeep([](const std::string& origin) { return origin.find("ours") != std::string::npos; });
  int64_t at = 1;
  ASSERT_EQ(store.Expect("https://ours.example/api/relay", "QmOurs", at++).outcome, PinOutcome::Pinned);
  for (size_t i = 1; i < RelayPinStore::kMaxPins; ++i) {
    store.Expect("https://seen" + std::to_string(i) + ".example/api/relay", "QmSeen" + std::to_string(i), at++);
  }
  ASSERT_EQ(store.All().size(), RelayPinStore::kMaxPins);
  store.Expect("https://new.example/api/relay", "QmNew", at++);
  EXPECT_EQ(store.All().size(), RelayPinStore::kMaxPins);
  EXPECT_TRUE(store.PinFor("https://ours.example/api/relay")) << "ours is kept, though it is the oldest";
  EXPECT_FALSE(store.PinFor("https://seen1.example/api/relay")) << "the oldest other goes";
  EXPECT_TRUE(store.PinFor("https://new.example/api/relay"));
  EXPECT_EQ(store.Present("https://other.example/api/relay", "QmOther", at, false).outcome, PinOutcome::NotExpected)
      << "an answer pins nothing where first use is not allowed";
}

// A store full of kept pins pins nothing, and says so: Add reads it as no identity, never as expected.
TEST_F(RelayPinsTest, PinsFullOfKeptRelaysPinNothingAndAddIsRefused) {
  pins_->SetKeep([](const std::string&) { return true; });
  for (size_t i = 0; i < RelayPinStore::kMaxPins; ++i) {
    const ByteVector key(kMlDsa65PublicKeyBytes, static_cast<uint8_t>(0x80 + i));
    ASSERT_TRUE(pins_->Expect("https://kept" + std::to_string(i) + ".example/api/relay",
                              pp::PeerIdFromMlDsaPublicKey(key).value()));
  }
  presents_[kA] = kPeerA;
  EXPECT_EQ(pins_->CheckForAdd(kA, kPeerA).result, AddIdentityCheck::Result::Unavailable);
  EXPECT_FALSE(pins_->PinFor(kA));
  auto audience = pins_->AudienceFor(kA)->PeerId();
  ASSERT_FALSE(audience) << "nothing signed for a relay not pinned";
  EXPECT_EQ(static_cast<Err::Network>(audience.error().code), Err::Network::RelayIdentityUnavailable);
}

TEST(RelayPinStoreTest, AnotherSchemaOrNotJsonIsRefusedAndReadAsEmpty) {
  EXPECT_FALSE(RelayPinsFromJson(R"({"schema_version":1,"pins":[]})")) << "MR007's first-come pins: not ours to read";
  EXPECT_FALSE(RelayPinsFromJson(R"({"schema_version":3,"pins":[]})"));
  EXPECT_FALSE(RelayPinsFromJson("not json"));
  const std::vector<RelayPin> pins{{.origin = "https://a.example/api/relay",
                                    .peer_id = kPeerA,
                                    .pinned_at_ms = 5,
                                    .presented_peer_id = kPeerB,
                                    .presented_at_ms = 6}};
  EXPECT_EQ(RelayPinsFromJson(RelayPinsToJson(pins)).value(), pins);

  const auto dir = std::filesystem::temp_directory_path() / "pp_relay_pins_schema";
  std::filesystem::create_directories(dir);
  const std::string newer = R"({"schema_version":9,"pins":[{"origin":"x","peer_id":"y"}]})";
  std::ofstream(dir / "relay_pins.json") << newer;
  RelayPinStore store(dir.string());
  store.Load();
  EXPECT_TRUE(store.All().empty()) << "expected again from their addresses";
  ASSERT_EQ(store.Expect("https://a.example/api/relay", kPeerA, 1).outcome, PinOutcome::Pinned);
  EXPECT_TRUE(store.PinFor("https://a.example/api/relay")) << "held in memory";
  std::string on_disk;
  {
    std::ifstream in(dir / "relay_pins.json"); // closed before the remove: Windows refuses to delete an open file
    on_disk.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  EXPECT_EQ(on_disk, newer) << "a newer app's file is never overwritten";
  std::filesystem::remove_all(dir);
}

TEST(RelayPinStoreTest, OnDiskTheRulesHoldToo) {
  const auto pins = RelayPinsFromJson(R"({"schema_version":2,"pins":[
      {"origin":"https://a.example/api/relay","peer_id":"QmA","pinned_at_ms":1},
      {"origin":"https://A.example:443/api/relay/","peer_id":"QmB","pinned_at_ms":2},
      {"origin":"https://c.example/api/relay","peer_id":"QmA","pinned_at_ms":3}]})");
  ASSERT_TRUE(pins);
  ASSERT_EQ(pins->size(), 1u) << "one pin per origin, one origin per PeerId";
  EXPECT_EQ(pins->front().peer_id, "QmA");
}

} // namespace
} // namespace pbr
