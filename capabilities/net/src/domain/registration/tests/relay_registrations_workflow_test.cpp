#include "domain/registration/RelayRegistrationsWorkflow.h"

#include "foundation/crypto/CryptoConstants.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/IdentityStore.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

// Our account on our other relays (multi-relay R2; SERVICE_ENDPOINTS § Several relays): registered there with our card
// once registered on the preferred relay, again for a newer card, near its expiry and after a backoff when it failed;
// the identity keeps the preferred relay's registration only.

constexpr const char kOther[] = "https://b.example/api/relay";
constexpr int64_t kNow = 1'704'067'200'000; // 2024-01-01T00:00:00Z

class CardRecordingClient : public MockRegistrationClient {
public:
  Roe<RegistrationResult> FinishRegistration(const std::string& challenge, const std::string& public_key_b64,
                                             const std::string& nickname, const std::string& signature,
                                             int64_t timestamp, const std::string& signature_alg,
                                             const std::string& kem_public_key_b64,
                                             const std::string& kem_binding_signature_b64, const std::string& peer_id,
                                             const std::vector<std::string>& multiaddrs,
                                             const RegistrationPublishOpts& publish) override {
    ++finish_calls;
    last_card = publish.account_card;
    if (fail) {
      return Error("relay down");
    }
    auto result = MockRegistrationClient::FinishRegistration(challenge, public_key_b64, nickname, signature, timestamp,
                                                             signature_alg, kem_public_key_b64,
                                                             kem_binding_signature_b64, peer_id, multiaddrs, publish);
    if (result) {
      result->expires_at = expires_at;
      result->llm_api_key = llm_key;
    }
    return result;
  }
  Roe<void> RevokeRegistration(const std::string&, const std::string&, int64_t) override {
    ++revoke_calls;
    return {};
  }
  bool fail = false;
  /** What the relay answers as the Brief LLM key (only the org's relay's client keeps one, MR006). */
  std::string llm_key;
  std::string expires_at = "2099-01-01T00:00:00.000Z";
  int finish_calls = 0;
  int revoke_calls = 0;
  std::optional<Object> last_card;
};

RegistrationPublishOpts CardIssuedAt(int64_t issued_at) {
  Object card;
  card.set("card_version", int64_t{3});
  card.set("issued_at", issued_at);
  return RegistrationPublishOpts{.account_card = card};
}

class RelayRegistrationsWorkflowTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_relay_registrations_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    identity_ = std::make_unique<IdentityStore>(dir_.string(), "test-profile");
    ASSERT_TRUE(static_cast<bool>(identity_->SetDek(ByteVector(kDataEncryptionKeySize, 7))));
    ASSERT_TRUE(static_cast<bool>(identity_->LoadOrCreate()));
    workflow_ = std::make_unique<RelayRegistrationsWorkflow>(*identity_);
    workflow_->SetRelays({{kOther, client_}});
    workflow_->SetCard([this]() { return card_; });
    workflow_->SetOnRegistered([this](const std::string& origin) { registered_on_.push_back(origin); });
  }
  void TearDown() override {
    workflow_.reset();
    identity_.reset();
    std::filesystem::remove_all(dir_);
  }

  void RegisterOnPreferred() {
    LocalIdentity identity = identity_->Get().value();
    identity.registered = true;
    identity.registration_expires_at = "2099-01-01T00:00:00.000Z";
    identity.registered_backend_url = "https://a.example/api/relay";
    ASSERT_TRUE(static_cast<bool>(identity_->Update(identity)));
  }

  std::filesystem::path dir_;
  std::unique_ptr<IdentityStore> identity_;
  std::shared_ptr<CardRecordingClient> client_ = std::make_shared<CardRecordingClient>();
  RegistrationPublishOpts card_ = CardIssuedAt(1000);
  std::vector<std::string> registered_on_;
  std::unique_ptr<RelayRegistrationsWorkflow> workflow_;
};

TEST_F(RelayRegistrationsWorkflowTest, NothingIsRegisteredBeforeThePreferredRelayIs) {
  EXPECT_TRUE(workflow_->Due(kNow).empty());
  workflow_->RunDue(kNow);
  EXPECT_EQ(client_->finish_calls, 0);
}

TEST_F(RelayRegistrationsWorkflowTest, EachOtherRelayGetsOurRegistrationWithOurCard) {
  RegisterOnPreferred();
  workflow_->RunDue(kNow);
  ASSERT_EQ(client_->finish_calls, 1);
  ASSERT_TRUE(client_->last_card);
  EXPECT_EQ(client_->last_card->getIf<int64_t>("issued_at"), 1000);
  EXPECT_TRUE(workflow_->Registered(kOther));
  EXPECT_EQ(registered_on_, std::vector<std::string>({kOther})) << "its push binding may go now";

  const LocalIdentity identity = identity_->Get().value();
  EXPECT_EQ(identity.registered_backend_url, "https://a.example/api/relay") << "the identity keeps the preferred's";
  EXPECT_EQ(identity.registered_card_issued_at, 0);

  workflow_->RunDue(kNow + 1000);
  EXPECT_EQ(client_->finish_calls, 1) << "nothing changed: not again";
}

TEST_F(RelayRegistrationsWorkflowTest, ANewerCardGoesAgain) {
  RegisterOnPreferred();
  workflow_->RunDue(kNow);
  card_ = CardIssuedAt(2000);
  workflow_->RunDue(kNow + 1000);
  ASSERT_EQ(client_->finish_calls, 2);
  EXPECT_EQ(client_->last_card->getIf<int64_t>("issued_at"), 2000);
}

TEST_F(RelayRegistrationsWorkflowTest, ARegistrationNearItsEndIsRenewed) {
  RegisterOnPreferred();
  client_->expires_at = "2024-01-10T00:00:00.000Z"; // within 14 days of kNow
  workflow_->RunDue(kNow);
  workflow_->RunDue(kNow + 1000);
  EXPECT_EQ(client_->finish_calls, 2);
}

TEST_F(RelayRegistrationsWorkflowTest, AFailureIsTriedAgainAfterABackoffThatDoubles) {
  RegisterOnPreferred();
  client_->fail = true;
  workflow_->RunDue(kNow);
  EXPECT_FALSE(workflow_->Registered(kOther));
  workflow_->RunDue(kNow + RelayRegistrationsWorkflow::kFirstRetryMs - 1);
  EXPECT_EQ(client_->finish_calls, 1);
  workflow_->RunDue(kNow + RelayRegistrationsWorkflow::kFirstRetryMs);
  EXPECT_EQ(client_->finish_calls, 2);
  const int64_t second = kNow + RelayRegistrationsWorkflow::kFirstRetryMs;
  workflow_->RunDue(second + 2 * RelayRegistrationsWorkflow::kFirstRetryMs - 1);
  EXPECT_EQ(client_->finish_calls, 2);
  client_->fail = false;
  workflow_->RunDue(second + 2 * RelayRegistrationsWorkflow::kFirstRetryMs);
  EXPECT_EQ(client_->finish_calls, 3);
  EXPECT_TRUE(workflow_->Registered(kOther));
}

TEST_F(RelayRegistrationsWorkflowTest, RevokeAllRemovesUsFromEachRelay) {
  RegisterOnPreferred();
  workflow_->RunDue(kNow);
  workflow_->RevokeAll();
  EXPECT_EQ(client_->revoke_calls, 1);
  EXPECT_FALSE(workflow_->Registered(kOther));
}

TEST_F(RelayRegistrationsWorkflowTest, ARelayStillListedKeepsWhatItAnswered) {
  RegisterOnPreferred();
  workflow_->RunDue(kNow);
  workflow_->SetRelays({{kOther, client_}, {"https://c.example/api/relay", std::make_shared<CardRecordingClient>()}});
  EXPECT_TRUE(workflow_->Registered(kOther));
  EXPECT_EQ(workflow_->Due(kNow + 1000), std::vector<std::string>({"https://c.example/api/relay"}));
}

// The Brief LLM key is the org's (MR006): when the org's relay is another of ours (the user prefers another), the key
// it answers with is the identity's; another relay's client answers with none (HttpRegistrationClient).
TEST_F(RelayRegistrationsWorkflowTest, TheOrgsRelayAsAnotherOfOursKeepsTheBriefLlmKey) {
  RegisterOnPreferred();
  client_->llm_key = "brf_llm_org";
  workflow_->RunDue(kNow);
  EXPECT_EQ(identity_->Get().value().brief_llm_api_key.Reveal(), "brf_llm_org");
}

TEST_F(RelayRegistrationsWorkflowTest, ARelayWithoutAKeyLeavesTheIdentitysAlone) {
  RegisterOnPreferred();
  LocalIdentity identity = identity_->Get().value();
  identity.brief_llm_api_key = std::string("brf_llm_kept");
  ASSERT_TRUE(static_cast<bool>(identity_->Update(identity)));
  workflow_->RunDue(kNow);
  EXPECT_EQ(identity_->Get().value().brief_llm_api_key.Reveal(), "brf_llm_kept");
}

// What each relay answered is kept (My relays shows it after a restart) and given back at the next start: a relay
// that holds our registration for the current card is not asked again.
TEST_F(RelayRegistrationsWorkflowTest, EachAnswerIsReportedToKeep) {
  RegisterOnPreferred();
  std::vector<RelayRegistrationRecord> records;
  workflow_->SetOnResult(
      [&records](const std::string&, const RelayRegistrationRecord& record) { records.push_back(record); });
  client_->fail = true;
  workflow_->RunDue(kNow);
  client_->fail = false;
  workflow_->RunDue(kNow + RelayRegistrationsWorkflow::kFirstRetryMs);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].state, RelayRegistrationRecord::State::Failed);
  EXPECT_EQ(records[0].reason, "relay down");
  EXPECT_EQ(records[1].state, RelayRegistrationRecord::State::Registered);
  EXPECT_EQ(records[1].expires_at, "2099-01-01T00:00:00.000Z");
  EXPECT_EQ(records[1].card_issued_at, 1000);
}

TEST_F(RelayRegistrationsWorkflowTest, ARegistrationKeptFromBeforeTheStartIsNotAskedAgain) {
  RegisterOnPreferred();
  RelayRegistrationsWorkflow restarted(*identity_);
  restarted.SetCard([this]() { return card_; });
  restarted.SetRelays({{.origin = kOther,
                        .client = client_,
                        .last = {.state = RelayRegistrationRecord::State::Registered,
                                 .expires_at = "2099-01-01T00:00:00.000Z",
                                 .card_issued_at = 1000}}});
  EXPECT_TRUE(restarted.Registered(kOther)) << "its inbox is polled at once";
  restarted.RunDue(kNow);
  EXPECT_EQ(client_->finish_calls, 0);
  card_ = CardIssuedAt(2000);
  restarted.RunDue(kNow);
  EXPECT_EQ(client_->finish_calls, 1) << "a newer card goes";
}

TEST_F(RelayRegistrationsWorkflowTest, ARelayLeavingTheListIsRevokedThere) {
  RegisterOnPreferred();
  workflow_->RunDue(kNow);
  ASSERT_TRUE(workflow_->Registered(kOther));
  ASSERT_TRUE(static_cast<bool>(workflow_->Revoke(kOther)));
  EXPECT_EQ(client_->revoke_calls, 1);
  EXPECT_FALSE(workflow_->Registered(kOther));
  EXPECT_FALSE(static_cast<bool>(workflow_->Revoke("https://z.example/api/relay")));
}

} // namespace
} // namespace pbr
