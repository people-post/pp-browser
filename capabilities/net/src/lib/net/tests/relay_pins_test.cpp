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
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pbr {
namespace {

// MR007-PIN: a pin comes only from an origin's own identity answer (or our own card on a linked device); one PeerId per
// origin and one origin per PeerId; a changed identity gets no signed call until accepted
// (docs/contracts/SERVICE_ENDPOINTS.md § Relay identity, DATA_LAYOUT.md `relay_pins.json`).

constexpr const char kA[] = "https://a.example/api/relay";
constexpr const char kB[] = "https://b.example/api/relay";
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
    pins_->SetOnChanged([this](const std::string& origin) { changed_.push_back(origin); });
    pins_->SetOnNotRegistered([this](const std::string& origin) { lost_.push_back(origin); });
  }

  std::filesystem::path dir_;
  int64_t now_ms_ = 1'761'235'200'000;
  std::map<std::string, std::string> presents_;
  std::vector<std::string> fetched_;
  std::vector<bool> public_only_;
  std::vector<std::string> changed_;
  std::vector<std::string> lost_;
  std::shared_ptr<RelayPins> pins_;
};

TEST_F(RelayPinsTest, TheFirstFetchPinsBeforeAnythingIsSigned) {
  presents_[kA] = kPeerA;
  auto audience = pins_->AudienceFor(kA);
  EXPECT_EQ(audience->PeerId().valueOr(""), kPeerA);
  EXPECT_EQ(fetched_, std::vector<std::string>{kA});
  EXPECT_EQ(changed_, std::vector<std::string>{kA});
  EXPECT_EQ(audience->PeerId().valueOr(""), kPeerA);
  EXPECT_EQ(fetched_.size(), 1u) << "pinned: not asked again";
  EXPECT_EQ(pins_->AudienceFor("HTTPS://A.example:443/api/relay/"), audience) << "one audience per relay";

  Open(); // restarted: the pin is profile data
  EXPECT_EQ(pins_->PinnedPeerId(kA).value_or(""), kPeerA);
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), kPeerA);
  EXPECT_EQ(fetched_.size(), 1u);
}

// A relay not ours (a QR locator's) is asked for its identity as its client asks it: https to a public address only.
TEST_F(RelayPinsTest, ARelayNotOursIsAskedOverAPublicAddressOnlyAndNeverPinned) {
  presents_[kA] = kPeerA;
  presents_[kB] = kPeerB;
  auto ours = pins_->AudienceFor(kA);
  auto theirs = pins_->ForeignAudienceFor(kB, true);
  EXPECT_EQ(ours->PeerId().valueOr(""), kPeerA);
  EXPECT_EQ(theirs->PeerId().valueOr(""), kPeerB);
  EXPECT_EQ(fetched_, (std::vector<std::string>{kA, kB}));
  EXPECT_EQ(public_only_, (std::vector<bool>{false, true}));
  EXPECT_FALSE(pins_->PinFor(kB)) << "what a relay not ours presents is never pinned";
  EXPECT_EQ(changed_, std::vector<std::string>{kA});
  EXPECT_EQ(theirs->PeerId().valueOr(""), kPeerB);
  EXPECT_EQ(fetched_.size(), 2u) << "kept in memory for its sends";

  presents_[kB] = kPeerC;
  theirs->Refused("wrong_audience");
  EXPECT_EQ(theirs->PeerId().valueOr(""), kPeerC) << "a wrong_audience asks again";
  theirs->Refused("not_registered");
  EXPECT_TRUE(lost_.empty()) << "our registration at a relay not ours is not ours to heal";
}

// S1: a QR code's relay that presents our relay's PeerId before ours is pinned takes nothing from ours.
TEST_F(RelayPinsTest, ALocatorsRelayClaimingOurPeerIdNeverBlocksOurRelay) {
  presents_[kB] = kPeerA; // the invite's relay claims A's identity
  presents_[kA] = kPeerA;
  ASSERT_TRUE(pins_->ForeignAudienceFor(kB, true)->PeerId()) << "nothing of ours holds kPeerA yet";
  EXPECT_FALSE(pins_->PinFor(kB));
  EXPECT_EQ(pins_->CheckCardRelay(kA, kPeerA), CardRelayCheck::Usable) << "contacts on the real relay stay usable";
  EXPECT_EQ(pins_->CheckForAdd(kA).result, AddIdentityCheck::Result::Ok) << "Add of the real relay goes through";
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), kPeerA);

  auto squatter = pins_->ForeignAudienceFor(kB, true)->PeerId();
  ASSERT_FALSE(squatter) << "once ours holds it, the squatter gets no call signed for it";
  EXPECT_EQ(static_cast<Err::Auth>(squatter.error().code), Err::Auth::RelayIdentityTaken);

  EXPECT_EQ(pins_->ForeignAudienceFor(kA, true)->PeerId().valueOr(""), kPeerA)
      << "a locator naming a relay of ours signs for its pin";
}

TEST_F(RelayPinsTest, AnAnswerThatDoesNotCheckOutPinsNothingAndIsAskedAgainAfterABackoff) {
  auto audience = pins_->AudienceFor(kA);
  ASSERT_FALSE(audience->PeerId());
  EXPECT_FALSE(pins_->PinFor(kA));
  EXPECT_FALSE(audience->PeerId()) << "within the backoff: nothing signed, nothing asked";
  EXPECT_EQ(fetched_.size(), 1u);
  presents_[kA] = kPeerA;
  now_ms_ += RelayPins::kFirstRetryMs;
  EXPECT_EQ(audience->PeerId().valueOr(""), kPeerA);
  EXPECT_EQ(fetched_.size(), 2u);
}

TEST_F(RelayPinsTest, AnotherPeerIdAfterWrongAudienceIsAnIdentityChangeUntilAccepted) {
  presents_[kA] = kPeerA;
  auto audience = pins_->AudienceFor(kA);
  ASSERT_TRUE(audience->PeerId());
  presents_[kA] = kPeerB; // the relay rotated its key
  changed_.clear();
  audience->Refused("wrong_audience");
  EXPECT_TRUE(pins_->IdentityChanged(kA));
  EXPECT_EQ(changed_, std::vector<std::string>{kA});
  auto refused = audience->PeerId();
  ASSERT_FALSE(refused) << "no signed call until the person accepts";
  EXPECT_EQ(static_cast<Err::Auth>(refused.error().code), Err::Auth::RelayIdentityChanged);
  EXPECT_FALSE(pins_->PinnedPeerId(kA)) << "left off our card meanwhile";
  EXPECT_EQ(pins_->PinFor(kA)->peer_id, kPeerA) << "the pin keeps the old PeerId";

  Open(); // the state survives a restart
  EXPECT_TRUE(pins_->IdentityChanged(kA));
  ASSERT_TRUE(pins_->Accept(kA));
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), kPeerB);
  EXPECT_FALSE(pins_->IdentityChanged(kA));
}

TEST_F(RelayPinsTest, WrongAudienceForThePinnedPeerIdIsAMisbehavingRelayAndAskedAtMostEveryFiveMinutes) {
  presents_[kA] = kPeerA;
  auto audience = pins_->AudienceFor(kA);
  ASSERT_TRUE(audience->PeerId());
  audience->Refused("wrong_audience");
  audience->Refused("wrong_audience");
  EXPECT_EQ(fetched_.size(), 2u) << "one identity fetch for the two refusals";
  EXPECT_FALSE(pins_->IdentityChanged(kA));
  EXPECT_EQ(audience->PeerId().valueOr(""), kPeerA);
  now_ms_ += RelayPins::kWrongAudienceRefetchMs;
  audience->Refused("wrong_audience");
  EXPECT_EQ(fetched_.size(), 3u);
}

TEST_F(RelayPinsTest, APeerIdHeldByAnotherOriginIsRefusedAndNeverPinned) {
  presents_[kA] = kPeerA;
  presents_[kB] = kPeerA; // B claims A's identity
  ASSERT_TRUE(pins_->AudienceFor(kA)->PeerId());
  auto taken = pins_->AudienceFor(kB)->PeerId();
  ASSERT_FALSE(taken);
  EXPECT_EQ(static_cast<Err::Auth>(taken.error().code), Err::Auth::RelayIdentityTaken);
  EXPECT_FALSE(pins_->PinFor(kB));
  const AddIdentityCheck add = pins_->CheckForAdd(kB);
  EXPECT_EQ(add.result, AddIdentityCheck::Result::Taken);
  EXPECT_EQ(add.holder, "https://a.example/api/relay");
  EXPECT_FALSE(pins_->Repin(kB, kPeerA)) << "not even on confirm";

  pins_->Drop(kA); // Remove… on A: its pin goes, and B's claim no longer collides
  EXPECT_EQ(pins_->CheckForAdd(kB).result, AddIdentityCheck::Result::Ok);
}

TEST_F(RelayPinsTest, AddAsksAgainForARelaySeenWithAnotherIdentity) {
  presents_[kA] = kPeerA;
  ASSERT_TRUE(pins_->AudienceFor(kA)->PeerId()); // the org's origin, or a relay of ours removed since
  presents_[kA] = kPeerB;
  const AddIdentityCheck add = pins_->CheckForAdd(kA);
  EXPECT_EQ(add.result, AddIdentityCheck::Result::ChangedSinceSeen);
  EXPECT_EQ(add.peer_id, kPeerB);
  EXPECT_EQ(pins_->PinnedPeerId(kA).value_or(""), kPeerA) << "nothing re-pinned without the person";
  ASSERT_TRUE(pins_->Repin(kA, add.peer_id));
  EXPECT_EQ(pins_->PinnedPeerId(kA).value_or(""), kPeerB);
  presents_.erase(kB);
  EXPECT_EQ(pins_->CheckForAdd(kB).result, AddIdentityCheck::Result::Unavailable)
      << "a server that does not identify itself as a relay";
}

TEST_F(RelayPinsTest, AContactsCardWritesNoPinButIsCheckedAgainstThem) {
  presents_[kA] = kPeerA;
  ASSERT_TRUE(pins_->AudienceFor(kA)->PeerId());
  EXPECT_EQ(pins_->CheckCardRelay(kA, kPeerA), CardRelayCheck::Usable);
  EXPECT_EQ(pins_->CheckCardRelay(kA, kPeerB), CardRelayCheck::Mismatch) << "our pin for that origin is another";
  EXPECT_EQ(pins_->CheckCardRelay(kB, kPeerA), CardRelayCheck::Mismatch) << "that PeerId is pinned to another origin";
  EXPECT_EQ(pins_->CheckCardRelay(kB, kPeerC), CardRelayCheck::Usable) << "no pin for either: the card's word";
  EXPECT_FALSE(pins_->PinFor(kB)) << "checking writes nothing";
}

TEST_F(RelayPinsTest, OurOwnCardOnALinkedDeviceWritesPins) {
  presents_[kA] = kPeerA;
  ASSERT_TRUE(pins_->AudienceFor(kA)->PeerId());
  // Our own account says B holds kPeerA now (the relay moved) and A holds kPeerC.
  pins_->TakeOwnCard({{.origin = kB, .peer_id = kPeerA}, {.origin = kA, .peer_id = kPeerC}});
  EXPECT_EQ(pins_->PinnedPeerId(kB).value_or(""), kPeerA);
  EXPECT_EQ(pins_->PinnedPeerId(kA).value_or(""), kPeerC);
  EXPECT_TRUE(fetched_.size() == 1u) << "written from the card, not fetched";
}

TEST_F(RelayPinsTest, NotRegisteredGoesToTheOwnerAndWrongAudienceDoesNot) {
  presents_[kA] = kPeerA;
  auto audience = pins_->AudienceFor(kA);
  audience->Refused("not_registered");
  audience->Refused("registration_expired");
  audience->Refused("something_else");
  EXPECT_EQ(lost_, (std::vector<std::string>{kA, kA}));
}

TEST_F(RelayPinsTest, OurCardsRelaysArePinnedOnesInForceInOrder) {
  presents_[kA] = kPeerA;
  presents_[kB] = kPeerB;
  ASSERT_TRUE(pins_->AudienceFor(kA)->PeerId());
  const auto relays = pins_->PinnedRelays({kB, kA});
  ASSERT_EQ(relays.size(), 1u) << "B is not pinned yet: left off";
  EXPECT_EQ(relays[0].origin, kA);
  EXPECT_EQ(relays[0].peer_id, kPeerA);
  EXPECT_TRUE(pins_->EnsurePinned({kB, kA}));
  EXPECT_EQ(pins_->PinnedRelays({kB, kA}).size(), 2u);
}

TEST(RelayPinStoreTest, TheBoundKeepsOursAndDropsTheOldestOther) {
  RelayPinStore store("");
  store.SetKeep([](const std::string& origin) { return origin.find("ours") != std::string::npos; });
  int64_t at = 1;
  ASSERT_EQ(store.Present("https://ours.example/api/relay", "QmOurs", at++).outcome, PinOutcome::Pinned);
  for (size_t i = 1; i < RelayPinStore::kMaxPins; ++i) {
    store.Present("https://seen" + std::to_string(i) + ".example/api/relay", "QmSeen" + std::to_string(i), at++);
  }
  ASSERT_EQ(store.All().size(), RelayPinStore::kMaxPins);
  store.Present("https://new.example/api/relay", "QmNew", at++);
  EXPECT_EQ(store.All().size(), RelayPinStore::kMaxPins);
  EXPECT_TRUE(store.PinFor("https://ours.example/api/relay")) << "ours is kept, though it is the oldest";
  EXPECT_FALSE(store.PinFor("https://seen1.example/api/relay")) << "the oldest other goes";
  EXPECT_TRUE(store.PinFor("https://new.example/api/relay"));
}

TEST(RelayPinStoreTest, ANewerSchemaOrNotJsonIsRefusedAndReadAsEmpty) {
  EXPECT_FALSE(RelayPinsFromJson(R"({"schema_version":2,"pins":[]})"));
  EXPECT_FALSE(RelayPinsFromJson("not json"));
  const std::vector<RelayPin> pins{{.origin = "https://a.example/api/relay",
                                    .peer_id = kPeerA,
                                    .pinned_at_ms = 5,
                                    .presented_peer_id = kPeerB,
                                    .presented_at_ms = 6}};
  EXPECT_EQ(RelayPinsFromJson(RelayPinsToJson(pins)).value(), pins);

  const auto dir = std::filesystem::temp_directory_path() / "pp_relay_pins_schema";
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "relay_pins.json") << R"({"schema_version":9,"pins":[{"origin":"x","peer_id":"y"}]})";
  RelayPinStore store(dir.string());
  store.Load();
  EXPECT_TRUE(store.All().empty()) << "pinned again on first use (TOFU once more)";
  std::filesystem::remove_all(dir);
}

TEST(RelayPinStoreTest, OnDiskTheRulesHoldToo) {
  const auto pins = RelayPinsFromJson(R"({"schema_version":1,"pins":[
      {"origin":"https://a.example/api/relay","peer_id":"QmA","pinned_at_ms":1},
      {"origin":"https://A.example:443/api/relay/","peer_id":"QmB","pinned_at_ms":2},
      {"origin":"https://c.example/api/relay","peer_id":"QmA","pinned_at_ms":3}]})");
  ASSERT_TRUE(pins);
  ASSERT_EQ(pins->size(), 1u) << "one pin per origin, one origin per PeerId";
  EXPECT_EQ(pins->front().peer_id, "QmA");
}

} // namespace
} // namespace pbr
