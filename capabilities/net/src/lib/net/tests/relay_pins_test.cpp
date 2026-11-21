#include "lib/net/RelayIdentity.h"
#include "lib/net/RelayPinStore.h"
#include "lib/net/RelayPins.h"
#include "lib/net/testing/TestRelayCertificates.h"

#include "common/ValueJson.h"
#include "foundation/error/AppError.h"

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

// RC020-RELAY (release-compatibility SIGNED_FORMATS § 3.4; docs/contracts/SERVICE_ENDPOINTS.md § Relay identity,
// § Pins; DATA_LAYOUT.md `relay_pins.json`): a relay address is its origin and its relay id; the relay id of a relay
// of ours is expected from its address (a relay link, the build, the config, our own card on a linked device), never
// from the relay's own answer but where first use is allowed; what the relay proves is its certificate, signed by its
// operator set: the highest valid one is kept per relay id and never a lower one, its transport key's PeerId is the
// audience, and it must list the origin it was asked at. A relay serving another relay's certificate, none valid, or
// one not listing it is not used until it proves its address again.

constexpr const char kA[] = "https://a.example/api/relay";
constexpr const char kB[] = "https://b.example/api/relay";
constexpr const char kC[] = "https://c.example/api/relay";
constexpr uint8_t kTransport1 = 0x61;
constexpr uint8_t kTransport2 = 0x62;
constexpr uint8_t kTransport3 = 0x63;

using test::TestRelay;

const TestRelay& RelayA() {
  static const TestRelay relay(0x11);
  return relay;
}
const TestRelay& RelayB() {
  static const TestRelay relay(0x12);
  return relay;
}

TEST(RelayIdentityTest, AnAnswerIsVersionTwoWithARelayIdAndACertificate) {
  const std::string certificate = RelayA().Certificate(1, {kA}, kTransport1);
  Object body;
  body.set("schema_version", int64_t{2});
  body.set("relay_id", RelayA().IdText());
  body.set("certificate_b64", certificate);
  body.set("something_new", true); // unknown keys are ignored
  auto identity = ParseRelayIdentity(DumpJson(body));
  ASSERT_TRUE(identity) << identity.error().message;
  EXPECT_EQ(identity->relay_id, RelayA().Id());
  EXPECT_EQ(identity->certificate_b64, certificate);

  Object newer = body;
  newer.set("schema_version", int64_t{3});
  EXPECT_TRUE(ParseRelayIdentity(DumpJson(newer))) << "T15: a newer schema is read for the fields this reader knows";
  for (const auto& [key, value] : std::vector<std::pair<std::string, Value>>{
           {"schema_version", int64_t{1}},
           {"relay_id", std::string("QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP")},
           {"relay_id", std::string("relay:short")},
           {"certificate_b64", std::string()}}) {
    Object bad = body;
    bad.set(key, value);
    const auto refused = ParseRelayIdentity(DumpJson(bad));
    ASSERT_FALSE(refused) << key;
    EXPECT_EQ(static_cast<Err::Network>(refused.error().code), Err::Network::RelayIdentityUnavailable)
        << "handled as no identity";
  }
  Object old;
  old.set("schema_version", int64_t{1});
  old.set("peer_id", std::string("QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP"));
  old.set("signature_alg", std::string("ml-dsa-65"));
  EXPECT_FALSE(ParseRelayIdentity(DumpJson(old))) << "a relay from before the certificate";
  EXPECT_FALSE(ParseRelayIdentity("not json"));
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
      const auto it = serves_.find(origin);
      if (it == serves_.end()) {
        return AppError::Network(Err::Network::RelayIdentityUnavailable, "relay_identity_unavailable: HTTP 503");
      }
      return it->second;
    });
    pins_->SetFirstUse([this](const std::string& origin) { return first_use_.contains(origin); });
    pins_->SetOnChanged([this](const std::string& origin) { changed_.push_back(origin); });
    pins_->SetOnNotRegistered([this](const std::string& origin) { lost_.push_back(origin); });
    pins_->SetOnUpgradeRequired([this](const std::string& origin) { too_old_.push_back(origin); });
  }
  /** `origin` serves `relay`'s certificate `sequence` for `transport`, listing `listed` (default: the origin). */
  void Serve(const std::string& origin, const TestRelay& relay, const uint64_t sequence, const uint8_t transport,
             std::vector<std::string> listed = {}) {
    if (listed.empty()) {
      listed = {origin};
    }
    serves_[origin] = relay.Identity(relay.Certificate(sequence, listed, transport));
  }

  std::filesystem::path dir_;
  int64_t now_ms_ = 1'795'000'000'000;
  std::map<std::string, RelayIdentity> serves_;
  std::set<std::string> first_use_;
  std::vector<std::string> fetched_;
  std::vector<bool> public_only_;
  std::vector<std::string> changed_;
  std::vector<std::string> lost_;
  std::vector<std::string> too_old_;
  std::shared_ptr<RelayPins> pins_;
};

TEST_F(RelayPinsTest, AnExpectedRelayIsSignedForTheTransportPeerIdItsCertificateProves) {
  ASSERT_TRUE(pins_->Expect(kA, RelayA().Id()));
  EXPECT_EQ(changed_, std::vector<std::string>{kA});
  Serve(kA, RelayA(), 1, kTransport1);
  auto audience = pins_->AudienceFor(kA);
  EXPECT_EQ(audience->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport1));
  EXPECT_EQ(fetched_, std::vector<std::string>{kA}) << "no certificate held: asked once, then held";
  EXPECT_EQ(audience->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport1));
  EXPECT_EQ(fetched_.size(), 1u);
  EXPECT_EQ(pins_->AudienceFor("HTTPS://A.example:443/api/relay/"), audience) << "one audience per relay";
  EXPECT_EQ(pins_->HeldAudience(kA).value_or(""), TestRelay::PeerIdOf(kTransport1));

  Open(); // restarted: the expectation and the certificate are profile data
  EXPECT_EQ(pins_->PinnedRelayId(kA).value_or(RelayId{}), RelayA().Id());
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport1));
  EXPECT_EQ(fetched_.size(), 1u) << "the held certificate: nothing asked";
  EXPECT_FALSE(pins_->Expect(kA, RelayId{})) << "only a relay id is expected";
}

// The review's attack (MR008 problem): without an expectation an origin's own answer is never taken (no first come).
TEST_F(RelayPinsTest, AnOriginNoAddressNamesIsNeverLearnedFromItsAnswer) {
  Serve(kA, RelayA(), 1, kTransport1);
  auto refused = pins_->AudienceFor(kA)->PeerId();
  ASSERT_FALSE(refused) << "nothing is signed for a relay whose address names no relay id";
  EXPECT_EQ(static_cast<Err::Network>(refused.error().code), Err::Network::RelayIdentityUnavailable);
  EXPECT_FALSE(pins_->CheckIdentities({kA}));
  EXPECT_TRUE(fetched_.empty()) << "not even asked";
  EXPECT_FALSE(pins_->PinFor(kA));
}

// § 3.4 step 4: a relay serving a certificate of another relay id, none valid, or one not listing its origin does not
// match its address: not used, never accepted, until it proves its address again.
TEST_F(RelayPinsTest, AnOriginNotProvingItsAddressIsNotUsedUntilItDoes) {
  ASSERT_TRUE(pins_->Expect(kA, RelayA().Id()));
  Serve(kA, RelayB(), 1, kTransport2); // another relay sits at the origin
  changed_.clear();
  EXPECT_TRUE(pins_->CheckIdentities({kA}));
  EXPECT_TRUE(pins_->IdentityMismatch(kA));
  EXPECT_EQ(pins_->PinFor(kA)->mismatch, RelayMismatch::OtherRelay);
  EXPECT_EQ(pins_->PinFor(kA)->presented_relay_id, RelayB().Id());
  EXPECT_EQ(changed_, std::vector<std::string>{kA});
  auto refused = pins_->AudienceFor(kA)->PeerId();
  ASSERT_FALSE(refused) << "not used";
  EXPECT_EQ(static_cast<Err::Auth>(refused.error().code), Err::Auth::RelayIdentityMismatch);
  EXPECT_FALSE(pins_->PinnedRelayId(kA)) << "left off our card meanwhile";
  EXPECT_EQ(pins_->PinFor(kA)->relay_id, RelayA().Id()) << "the address's relay id stays: the answer is never taken";

  Open(); // the state survives a restart
  EXPECT_TRUE(pins_->IdentityMismatch(kA));
  // Its own relay id, but a certificate that does not list the origin it was asked at: a relay fronting another.
  Serve(kA, RelayA(), 1, kTransport1, {kB});
  EXPECT_TRUE(pins_->CheckIdentities({kA}));
  EXPECT_EQ(pins_->PinFor(kA)->mismatch, RelayMismatch::OriginNotListed);
  // A certificate that does not verify (another relay's set signed it, served under this relay id).
  serves_[kA] = RelayIdentity{.relay_id = RelayA().Id(), .certificate_b64 = RelayB().Certificate(1, {kA}, 0x64)};
  EXPECT_TRUE(pins_->CheckIdentities({kA}));
  EXPECT_EQ(pins_->PinFor(kA)->mismatch, RelayMismatch::OtherRelay);
  serves_[kA] = RelayIdentity{.relay_id = RelayA().Id(), .certificate_b64 = "AQID"};
  EXPECT_TRUE(pins_->CheckIdentities({kA}));
  EXPECT_EQ(pins_->PinFor(kA)->mismatch, RelayMismatch::NoValidCertificate);

  Serve(kA, RelayA(), 1, kTransport1); // it proves its address again (its operator fixed the deployment)
  EXPECT_TRUE(pins_->CheckIdentities({kA}));
  EXPECT_FALSE(pins_->IdentityMismatch(kA));
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport1));
}

// § 3.2: per relay id the highest certificate seen is kept, from any channel; a lower one is never accepted, and
// another certificate at the sequence held is refused (the first stays).
TEST_F(RelayPinsTest, TheHighestCertificateIsKeptAndALowerOneNeverAccepted) {
  ASSERT_TRUE(pins_->Expect(kA, RelayA().Id()));
  const std::string one = RelayA().Certificate(1, {kA}, kTransport1);
  const std::string two = RelayA().Certificate(2, {kA}, kTransport2);
  EXPECT_EQ(pins_->OfferCertificate(RelayA().Id(), two, "client-compat").valueOr(RelayCertificateOffer::Conflict),
            RelayCertificateOffer::Adopted);
  EXPECT_EQ(pins_->OfferCertificate(RelayA().Id(), one, "the build").valueOr(RelayCertificateOffer::Conflict),
            RelayCertificateOffer::Lower);
  const std::string other_two = RelayA().Certificate(2, {kA}, kTransport3);
  EXPECT_EQ(pins_->OfferCertificate(RelayA().Id(), other_two, "client-compat").valueOr(RelayCertificateOffer::Adopted),
            RelayCertificateOffer::Conflict);
  EXPECT_FALSE(pins_->OfferCertificate(RelayB().Id(), two, "client-compat")) << "another relay's: refused";
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport2));
  EXPECT_TRUE(fetched_.empty()) << "the offered certificate gives the audience: nothing asked";

  serves_[kA] = RelayA().Identity(one); // the relay still serves its older certificate
  EXPECT_FALSE(pins_->CheckIdentities({kA})) << "a lower one changes nothing, and is no mismatch";
  EXPECT_FALSE(pins_->IdentityMismatch(kA));
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport2));
}

TEST_F(RelayPinsTest, EachRelayIsCheckedOncePerRunAndAgainWhileItDidNotAnswer) {
  ASSERT_TRUE(pins_->Expect(kA, RelayA().Id()));
  ASSERT_TRUE(pins_->Expect(kB, RelayB().Id()));
  Serve(kA, RelayA(), 1, kTransport1);
  EXPECT_TRUE(pins_->CheckIdentities({kA, kB})) << "A's certificate kept; B unanswered";
  EXPECT_EQ(fetched_, (std::vector<std::string>{kA, kB}));
  EXPECT_FALSE(pins_->CheckIdentities({kA, kB}));
  EXPECT_EQ(fetched_, (std::vector<std::string>{kA, kB, kB})) << "A checked already; B asked again";
  EXPECT_FALSE(pins_->AudienceFor(kB)->PeerId()) << "no certificate of B held: nothing is signed for it";
}

// § 3.4 step 4: `401 wrong_audience` asks again (at most once per relay per 5 min) and adopts a higher certificate.
TEST_F(RelayPinsTest, WrongAudienceAdoptsAHigherCertificateAndAsksAtMostEveryFiveMinutes) {
  ASSERT_TRUE(pins_->Expect(kA, RelayA().Id()));
  Serve(kA, RelayA(), 1, kTransport1);
  auto audience = pins_->AudienceFor(kA);
  ASSERT_EQ(audience->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport1));
  Serve(kA, RelayA(), 2, kTransport2); // the host moved: certificate 2 for its new transport key
  changed_.clear();
  audience->Refused("wrong_audience");
  audience->Refused("wrong_audience");
  EXPECT_EQ(fetched_.size(), 2u) << "one fetch for the two refusals";
  EXPECT_EQ(audience->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport2)) << "signed for the new PeerId";
  EXPECT_EQ(changed_, std::vector<std::string>{kA});
  EXPECT_FALSE(pins_->IdentityMismatch(kA));
  now_ms_ += RelayPins::kWrongAudienceRefetchMs;
  audience->Refused("wrong_audience");
  EXPECT_EQ(fetched_.size(), 3u);
}

// A link (or our own card) naming a relay that is ours already at another origin: the same relay twice, refused at Add.
TEST_F(RelayPinsTest, AddRefusesARelayOfOursAtAnotherOriginBeforeAskingIt) {
  ASSERT_TRUE(pins_->Expect(kA, RelayA().Id())); // the org's relay, from the build
  Serve(kB, RelayA(), 1, kTransport1, {kA});     // a hostile relay at B passes the org's answers through
  const AddIdentityCheck add = pins_->CheckForAdd(kB, RelayA().Id());
  EXPECT_EQ(add.result, AddIdentityCheck::Result::Taken);
  EXPECT_EQ(add.holder, "https://a.example/api/relay");
  EXPECT_TRUE(fetched_.empty()) << "refused before it is asked";
  pins_->Drop(kA);
  EXPECT_EQ(pins_->CheckForAdd(kB, RelayA().Id()).result, AddIdentityCheck::Result::Mismatch)
      << "the org's certificate does not list B: it cannot front the org";
}

TEST_F(RelayPinsTest, AddChecksTheCertificateAgainstTheLink) {
  Serve(kB, RelayA(), 1, kTransport1);
  const AddIdentityCheck mismatch = pins_->CheckForAdd(kB, RelayB().Id());
  EXPECT_EQ(mismatch.result, AddIdentityCheck::Result::Mismatch);
  EXPECT_FALSE(pins_->PinFor(kB)) << "nothing expected from an Add that did not go through";
  Serve(kB, RelayB(), 1, kTransport2);
  const AddIdentityCheck first = pins_->CheckForAdd(kB, RelayB().Id(), true);
  EXPECT_EQ(first.result, AddIdentityCheck::Result::Ok);
  EXPECT_TRUE(first.pinned_now);
  EXPECT_EQ(first.relay_id, RelayB().Id());
  EXPECT_TRUE(public_only_.back()) << "a relay the person added: asked at a public address only";
  EXPECT_EQ(pins_->HeldAudience(kB).value_or(""), TestRelay::PeerIdOf(kTransport2)) << "its certificate is held";
  EXPECT_FALSE(pins_->CheckForAdd(kB, RelayB().Id()).pinned_now) << "expected already";
  EXPECT_EQ(pins_->CheckForAdd(kB, RelayA().Id()).result, AddIdentityCheck::Result::Mismatch)
      << "an origin expected to prove another relay id";
  EXPECT_EQ(pins_->CheckForAdd(kC, RelayA().Id()).result, AddIdentityCheck::Result::Unavailable)
      << "a server that does not say which relay it is";
}

// MR008 item 4: a developer build's bare origin (and the org's in a build that names no relay id) on first use only.
TEST_F(RelayPinsTest, FirstUseOnlyWhereItIsAllowed) {
  Serve(kA, RelayA(), 1, kTransport1);
  Serve(kB, RelayB(), 1, kTransport2);
  first_use_.insert(kA);
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport1))
      << "pinned from its first valid certificate";
  EXPECT_EQ(pins_->PinFor(kA)->relay_id, RelayA().Id());
  EXPECT_FALSE(pins_->AudienceFor(kB)->PeerId()) << "first use is not allowed for B";
  EXPECT_EQ(fetched_, std::vector<std::string>{kA});

  const AddIdentityCheck bare = pins_->CheckForAdd(kB, RelayId{});
  EXPECT_EQ(bare.result, AddIdentityCheck::Result::Ok) << "Add of a bare origin (developer builds only)";
  EXPECT_EQ(bare.relay_id, RelayB().Id());
  EXPECT_TRUE(bare.pinned_now);
  Serve(kC, RelayA(), 2, kTransport1, {kA, kC}); // relay A's certificate 2 also lists C
  EXPECT_EQ(pins_->CheckForAdd(kC, RelayId{}).result, AddIdentityCheck::Result::Taken) << "relay A, ours at A";
}

TEST_F(RelayPinsTest, AFirstUseThatFailsPinsNothingAndIsAskedAgainAfterAShortBackoff) {
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
  Serve(kA, RelayA(), 1, kTransport1);
  EXPECT_FALSE(audience->PeerId()) << "within the backoff";
  EXPECT_TRUE(pins_->CheckIdentities({kA})) << "asked now, whatever the backoff";
  EXPECT_EQ(audience->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport1));
}

// § 3.4 step 5: a contact's relay is checked too: its certificate proves the PeerId; cached under its relay id, it
// never makes the relay ours.
TEST_F(RelayPinsTest, AContactsRelayIsSignedForWhatItsCertificateProvesAndWritesNoPin) {
  Serve(kB, RelayB(), 1, kTransport2);
  auto audience = pins_->ContactAudience(kB, RelayB().Id());
  EXPECT_EQ(audience->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport2));
  EXPECT_TRUE(public_only_.back()) << "a contact's relay: a public address only";
  EXPECT_FALSE(pins_->PinFor(kB)) << "never ours";
  EXPECT_TRUE(pins_->CertificateFor(RelayB().Id())) << "cached under its relay id";
  EXPECT_EQ(audience->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport2));
  EXPECT_EQ(fetched_.size(), 1u);

  auto wrong = pins_->ContactAudience(kB, RelayA().Id())->PeerId();
  ASSERT_FALSE(wrong) << "the card names a relay id B does not prove";
  EXPECT_EQ(static_cast<Err::Auth>(wrong.error().code), Err::Auth::RelayIdentityMismatch);
  EXPECT_FALSE(pins_->ContactAudience(kB, RelayA().Id())->PeerId());
  EXPECT_EQ(fetched_.size(), 2u) << "within the backoff: not asked again";

  Serve(kB, RelayB(), 2, kTransport3);
  audience->Refused("wrong_audience");
  EXPECT_EQ(audience->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport3)) << "a higher certificate adopted";
}

TEST_F(RelayPinsTest, AContactsCardEntryIsCheckedAgainstOurPinsAndCertificates) {
  ASSERT_TRUE(pins_->Expect(kA, RelayA().Id()));
  EXPECT_EQ(pins_->CheckCardRelay(kA, RelayA().Id()), CardRelayCheck::Usable);
  EXPECT_EQ(pins_->CheckCardRelay(kA, RelayB().Id()), CardRelayCheck::Mismatch) << "we expect another relay there";
  EXPECT_EQ(pins_->CheckCardRelay(kB, RelayB().Id()), CardRelayCheck::Usable) << "nothing known: the card's word";
  ASSERT_TRUE(pins_->OfferCertificate(RelayA().Id(), RelayA().Certificate(1, {kA}, kTransport1), "client-compat"));
  EXPECT_EQ(pins_->CheckCardRelay(kB, RelayA().Id()), CardRelayCheck::Mismatch)
      << "relay A's certificate does not list B";
  EXPECT_FALSE(pins_->PinFor(kB)) << "checking writes nothing";
}

TEST_F(RelayPinsTest, OurOwnCardOnALinkedDeviceWritesPins) {
  ASSERT_TRUE(pins_->Expect(kA, RelayA().Id()));
  pins_->TakeOwnCard({{.origin = kB, .relay_id = RelayA().Id()}, {.origin = kA, .relay_id = RelayB().Id()}});
  EXPECT_EQ(pins_->PinnedRelayId(kB).value_or(RelayId{}), RelayA().Id());
  EXPECT_EQ(pins_->PinnedRelayId(kA).value_or(RelayId{}), RelayB().Id());
  EXPECT_TRUE(fetched_.empty()) << "written from the card, not fetched";
}

TEST_F(RelayPinsTest, NotRegisteredGoesToTheOwnerAndWrongAudienceDoesNot) {
  ASSERT_TRUE(pins_->Expect(kA, RelayA().Id()));
  auto audience = pins_->AudienceFor(kA);
  audience->Refused("not_registered");
  audience->Refused("registration_expired");
  audience->Refused("something_else");
  EXPECT_EQ(lost_, (std::vector<std::string>{kA, kA}));
  EXPECT_TRUE(too_old_.empty());
}

// 426 `client_upgrade_required`: this app is too old for that relay — told to the owner each time, never taken for a
// lost registration or an identity question.
TEST_F(RelayPinsTest, UpgradeRequiredGoesToTheOwnerEachTime) {
  ASSERT_TRUE(pins_->Expect(kA, RelayA().Id()));
  auto audience = pins_->AudienceFor(kA);
  audience->Refused("client_upgrade_required");
  audience->Refused("client_upgrade_required");
  EXPECT_EQ(too_old_, (std::vector<std::string>{kA, kA}));
  EXPECT_TRUE(lost_.empty());
  EXPECT_TRUE(fetched_.empty()) << "its identity is not asked again";
}

// U11: a relay older than this app goes to its own owner hook, never to "this app is too old".
TEST_F(RelayPinsTest, ARelayOlderThanThisAppGoesToItsOwnHook) {
  std::vector<std::string> outdated;
  pins_->SetOnRelayOutdated([&outdated](const std::string& origin) { outdated.push_back(origin); });
  ASSERT_TRUE(pins_->Expect(kA, RelayA().Id()));
  pins_->AudienceFor(kA)->Refused("relay_upgrade_required");
  EXPECT_EQ(outdated, std::vector<std::string>{kA});
  EXPECT_TRUE(too_old_.empty());
  EXPECT_TRUE(lost_.empty());
  EXPECT_TRUE(fetched_.empty()) << "its identity is not asked again";
}

TEST_F(RelayPinsTest, OurCardsRelaysArePinnedOnesInForceInOrder) {
  ASSERT_TRUE(pins_->Expect(kA, RelayA().Id()));
  const auto relays = pins_->PinnedRelays({kB, kA});
  ASSERT_EQ(relays.size(), 1u) << "B names no relay id: left off";
  EXPECT_EQ(relays[0].origin, kA);
  EXPECT_EQ(relays[0].relay_id, RelayA().Id());
  ASSERT_TRUE(pins_->Expect(kB, RelayB().Id()));
  EXPECT_EQ(pins_->PinnedRelays({kB, kA}).size(), 2u);
}

// An expired certificate held gives no audience: the relay is asked for a newer one.
TEST_F(RelayPinsTest, AnExpiredCertificateIsNotSignedFor) {
  ASSERT_TRUE(pins_->Expect(kA, RelayA().Id()));
  ASSERT_TRUE(
      pins_->OfferCertificate(RelayA().Id(), RelayA().Certificate(1, {kA}, kTransport1, now_ms_ + 1000), "the build"));
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport1));
  now_ms_ += 2000;
  EXPECT_FALSE(pins_->HeldAudience(kA));
  Serve(kA, RelayA(), 2, kTransport2);
  EXPECT_EQ(pins_->AudienceFor(kA)->PeerId().valueOr(""), TestRelay::PeerIdOf(kTransport2));
}

// A store full of kept pins pins nothing, and says so: Add reads it as no identity, never as expected.
TEST_F(RelayPinsTest, PinsFullOfKeptRelaysPinNothingAndAddIsRefused) {
  pins_->SetKeep([](const std::string&) { return true; });
  for (size_t i = 0; i < RelayPinStore::kMaxPins; ++i) {
    ASSERT_TRUE(pins_->Expect("https://kept" + std::to_string(i) + ".example/api/relay",
                              RelayId::FromRaw(ByteVector(32, static_cast<uint8_t>(i))).value()));
  }
  Serve(kA, RelayA(), 1, kTransport1);
  EXPECT_EQ(pins_->CheckForAdd(kA, RelayA().Id()).result, AddIdentityCheck::Result::Unavailable);
  EXPECT_FALSE(pins_->PinFor(kA));
  auto audience = pins_->AudienceFor(kA)->PeerId();
  ASSERT_FALSE(audience) << "nothing signed for a relay not pinned";
  EXPECT_EQ(static_cast<Err::Network>(audience.error().code), Err::Network::RelayIdentityUnavailable);
}

TEST(RelayPinStoreTest, TheBoundKeepsOursAndDropsTheOldestOther) {
  RelayPinStore store("");
  store.SetKeep([](const std::string& origin) { return origin.find("ours") != std::string::npos; });
  int64_t at = 1;
  const auto id = [](const size_t i) { return RelayId::FromRaw(ByteVector(32, static_cast<uint8_t>(i))).value(); };
  ASSERT_EQ(store.Expect("https://ours.example/api/relay", id(200), at++), PinOutcome::Pinned);
  for (size_t i = 1; i < RelayPinStore::kMaxPins; ++i) {
    store.Expect("https://seen" + std::to_string(i) + ".example/api/relay", id(i), at++);
  }
  ASSERT_EQ(store.All().size(), RelayPinStore::kMaxPins);
  store.Expect("https://new.example/api/relay", id(201), at++);
  EXPECT_EQ(store.All().size(), RelayPinStore::kMaxPins);
  EXPECT_TRUE(store.PinFor("https://ours.example/api/relay")) << "ours is kept, though it is the oldest";
  EXPECT_FALSE(store.PinFor("https://seen1.example/api/relay")) << "the oldest other goes";
  EXPECT_TRUE(store.PinFor("https://new.example/api/relay"));
}

TEST(RelayPinStoreTest, CertificatesOfRelaysNoPinNamesGoFirstWhenFull) {
  RelayPinStore store("");
  std::vector<test::TestRelay> relays;
  for (uint8_t i = 0; i < RelayPinStore::kMaxCertificates + 1; ++i) {
    relays.emplace_back(static_cast<uint8_t>(0x40 + i));
  }
  ASSERT_EQ(store.Expect(kA, relays[0].Id(), 1), PinOutcome::Pinned);
  for (size_t i = 0; i < relays.size(); ++i) {
    const auto check = ReceiveRelayCertificateB64(relays[i].Certificate(1, {kA}, 0x01), relays[i].Id(), 0);
    ASSERT_TRUE(check.accepted()) << check.detail;
    EXPECT_EQ(store.Offer(*check.certificate, static_cast<int64_t>(i)), RelayCertificateOffer::Adopted);
  }
  EXPECT_TRUE(store.CertificateFor(relays[0].Id())) << "a pinned relay's certificate stays, though the oldest";
  EXPECT_FALSE(store.CertificateFor(relays[1].Id())) << "the oldest of a relay no pin names goes";
  EXPECT_TRUE(store.CertificateFor(relays.back().Id()));
}

TEST(RelayPinStoreTest, AnotherSchemaOrNotJsonIsRefusedAndReadAsEmpty) {
  EXPECT_FALSE(RelayPinsFromJson(R"({"schema_version":2,"pins":[]})")) << "MR008's PeerId pins: not read";
  EXPECT_FALSE(RelayPinsFromJson(R"({"schema_version":3,"pins":[]})")) << "no certificates list";
  EXPECT_FALSE(RelayPinsFromJson("not json"));
  const test::TestRelay& relay = RelayA();
  const auto held = ReceiveRelayCertificateB64(relay.Certificate(3, {kA}, kTransport1), relay.Id(), 0);
  ASSERT_TRUE(held.accepted());
  const RelayPinsFile file{.pins = {{.origin = "https://a.example/api/relay",
                                     .relay_id = relay.Id(),
                                     .pinned_at_ms = 5,
                                     .mismatch = RelayMismatch::OtherRelay,
                                     .presented_relay_id = RelayB().Id(),
                                     .mismatch_at_ms = 6}},
                           .certificates = {{.certificate = *held.certificate, .kept_at_ms = 7}}};
  const auto back = RelayPinsFromJson(RelayPinsToJson(file));
  ASSERT_TRUE(back);
  EXPECT_EQ(back->pins, file.pins);
  ASSERT_EQ(back->certificates.size(), 1u);
  EXPECT_EQ(back->certificates[0].certificate.blob, held.certificate->blob);
  EXPECT_EQ(back->certificates[0].kept_at_ms, 7);

  const auto dir = std::filesystem::temp_directory_path() / "pp_relay_pins_schema";
  std::filesystem::create_directories(dir);
  const std::string newer = R"({"schema_version":9,"pins":[{"origin":"x","relay_id":"y"}],"certificates":[]})";
  std::ofstream(dir / "relay_pins.json") << newer;
  RelayPinStore store(dir.string());
  store.Load();
  EXPECT_TRUE(store.All().empty()) << "expected again from their addresses";
  ASSERT_EQ(store.Expect("https://a.example/api/relay", relay.Id(), 1), PinOutcome::Pinned);
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
  const std::string a = RelayA().IdText();
  const std::string b = RelayB().IdText();
  const auto file =
      RelayPinsFromJson(R"({"schema_version":3,"pins":[
      {"origin":"https://a.example/api/relay","relay_id":")" +
                        a + R"(","pinned_at_ms":1},
      {"origin":"https://A.example:443/api/relay/","relay_id":")" +
                        b + R"(","pinned_at_ms":2},
      {"origin":"https://c.example/api/relay","relay_id":"QmNotARelayId","pinned_at_ms":3},
      {"origin":"https://d.example/api/relay","relay_id":")" +
                        a + R"(","pinned_at_ms":4}],
      "certificates":[{"relay_id":")" +
                        a + R"(","certificate_b64":")" + RelayB().Certificate(1, {kA}, kTransport1) + R"("}]})");
  ASSERT_TRUE(file);
  ASSERT_EQ(file->pins.size(), 2u) << "one pin per origin, a relay id of the form; a relay at two origins is fine";
  EXPECT_EQ(file->pins[0].relay_id, RelayA().Id());
  EXPECT_EQ(file->pins[1].origin, "https://d.example/api/relay");
  EXPECT_TRUE(file->certificates.empty()) << "a certificate that does not verify against its relay id is dropped";
}

} // namespace
} // namespace pbr
