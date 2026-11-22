#include "domain/registration/RelayRegistrationsWorkflow.h"

#include "foundation/error/AppError.h"

#include "foundation/crypto/CryptoConstants.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/IdentityStore.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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
                                             const std::string& nickname, const SignedRelayCommand& signed_finish,
                                             const std::string& kem_public_key_b64, const std::string& kem_binding_b64,
                                             const std::string& peer_id, const std::vector<std::string>& multiaddrs,
                                             const RegistrationPublishOpts& publish) override {
    ++finish_calls;
    events.push_back("finish");
    last_card = publish.account_card;
    if (on_finish) {
      on_finish();
    }
    if (fail) {
      return AppError::Network(Err::Network::ConnectFailed, "relay down");
    }
    if (too_old) {
      return AppError::Auth(Err::Auth::ClientUpgradeRequired, "client_upgrade_required");
    }
    if (refusal) {
      return *refusal;
    }
    auto result =
        MockRegistrationClient::FinishRegistration(challenge, public_key_b64, nickname, signed_finish,
                                                   kem_public_key_b64, kem_binding_b64, peer_id, multiaddrs, publish);
    if (result) {
      result->expires_at = expires_at;
      result->llm_token = llm_key;
      result->registration_id = "reg-" + std::to_string(finish_calls);
    }
    return result;
  }
  Roe<void> RevokeRegistration(const std::string&, const std::string& registration_id,
                               const SignedRelayCommand&) override {
    ++revoke_calls;
    events.push_back("revoke");
    revoked_ids.push_back(registration_id);
    return {};
  }
  std::vector<std::string> revoked_ids;
  bool fail = false;
  /** The relay answers 426 `client_upgrade_required` (this app is too old for it). */
  bool too_old = false;
  /** The relay refuses with this (typed as NoteRelayRefusal types it). */
  std::optional<Error> refusal;
  /** What the relay answers as the Brief LLM key (only the org's relay's client keeps one, MR006). */
  std::string llm_key;
  std::string expires_at = "2099-01-01T00:00:00.000Z";
  int finish_calls = 0;
  int revoke_calls = 0;
  /** finish / revoke, in the order the relay saw them. */
  std::vector<std::string> events;
  /** Runs inside register/finish (a relay that answers slowly). */
  std::function<void()> on_finish;
  std::optional<Object> last_card;
};

RegistrationPublishOpts CardIssuedAt(int64_t issued_at) {
  Object card;
  card.set("card_version", int64_t{4});
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
  EXPECT_EQ(client_->revoked_ids, std::vector<std::string>{"reg-1"}) << "the registration it answered (MR007-SCOPE)";
  EXPECT_FALSE(workflow_->Registered(kOther));
}

// MR007-SCOPE: a revoke names the registration the relay answered last — kept across a restart too.
TEST_F(RelayRegistrationsWorkflowTest, ARevokeNamesTheRegistrationItRemoves) {
  RegisterOnPreferred();
  workflow_->RunDue(kNow);
  card_ = CardIssuedAt(2000);
  workflow_->RunDue(kNow + 1000); // renewed: the relay answers its id again
  ASSERT_TRUE(static_cast<bool>(workflow_->Revoke(kOther)));
  EXPECT_EQ(client_->revoked_ids, std::vector<std::string>{"reg-2"});

  RelayRegistrationsWorkflow restarted(*identity_);
  restarted.SetRelays({{kOther, client_,
                        RelayRegistrationRecord{.state = RelayRegistrationRecord::State::Registered,
                                                .expires_at = "2099-01-01T00:00:00.000Z",
                                                .card_issued_at = 2000,
                                                .registration_id = "reg-kept"}}});
  ASSERT_TRUE(static_cast<bool>(restarted.Revoke(kOther)));
  EXPECT_EQ(client_->revoked_ids.back(), "reg-kept");
}

// MR007-HEAL: a relay of ours that answers not_registered while it held our registration is registered on again at
// once; lost again soon after, after a backoff that doubles.
TEST_F(RelayRegistrationsWorkflowTest, ALostRegistrationIsMadeAgainWithABackoff) {
  RegisterOnPreferred();
  workflow_->RunDue(kNow);
  ASSERT_EQ(client_->finish_calls, 1);

  ASSERT_TRUE(workflow_->Lost(kOther, kNow + 1000));
  EXPECT_FALSE(workflow_->Registered(kOther));
  workflow_->RunDue(kNow + 1000);
  EXPECT_EQ(client_->finish_calls, 2) << "due at once";

  ASSERT_TRUE(workflow_->Lost(kOther, kNow + 2000));
  workflow_->RunDue(kNow + 2000 + RegistrationLossBackoff::kFirstMs - 1);
  EXPECT_EQ(client_->finish_calls, 2) << "lost again soon after: after the backoff";
  workflow_->RunDue(kNow + 2000 + RegistrationLossBackoff::kFirstMs);
  EXPECT_EQ(client_->finish_calls, 3);

  const int64_t third = kNow + 3000 + RegistrationLossBackoff::kFirstMs;
  ASSERT_TRUE(workflow_->Lost(kOther, third));
  workflow_->RunDue(third + 2 * RegistrationLossBackoff::kFirstMs - 1);
  EXPECT_EQ(client_->finish_calls, 3) << "the backoff doubles";
  workflow_->RunDue(third + 2 * RegistrationLossBackoff::kFirstMs);
  EXPECT_EQ(client_->finish_calls, 4);
}

// MR007-HEAL: not for a relay leaving our list, nor one where we hold no registration.
TEST_F(RelayRegistrationsWorkflowTest, NoHealForALeavingOrUnregisteredRelay) {
  EXPECT_FALSE(workflow_->Lost(kOther, kNow)) << "never registered there";
  RegisterOnPreferred();
  workflow_->RunDue(kNow);
  ASSERT_TRUE(static_cast<bool>(workflow_->Revoke(kOther)));
  EXPECT_FALSE(workflow_->Lost(kOther, kNow + 1000)) << "leaving";
  workflow_->RunDue(kNow + 1000);
  EXPECT_EQ(client_->finish_calls, 1);
  EXPECT_FALSE(workflow_->Lost("https://not-ours.example/api/relay", kNow + 1000));
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
  EXPECT_EQ(records[0].reason, kRegistrationUnreachable) << "a code the page words, not the error's text";
  EXPECT_EQ(records[1].state, RelayRegistrationRecord::State::Registered);
  EXPECT_EQ(records[1].expires_at, "2099-01-01T00:00:00.000Z");
  EXPECT_EQ(records[1].card_issued_at, 1000);
}

// Another relay of ours newer than this app (426 `client_upgrade_required`): its row says it needs a newer version of
// the app, in a code the page words; nothing else is blocked.
TEST_F(RelayRegistrationsWorkflowTest, ARelayNeedingANewerAppSaysSo) {
  RegisterOnPreferred();
  std::vector<RelayRegistrationRecord> records;
  workflow_->SetOnResult(
      [&records](const std::string&, const RelayRegistrationRecord& record) { records.push_back(record); });
  client_->too_old = true;
  workflow_->RunDue(kNow);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].state, RelayRegistrationRecord::State::Failed);
  EXPECT_EQ(records[0].reason, kRegistrationUpgrade);
  EXPECT_TRUE(registered_on_.empty());
}

// T17: a refusal asking again will not change (this app too old there, the relay too old for it, another identity) is
// asked again once a day, not on the minute backoff.
TEST_F(RelayRegistrationsWorkflowTest, APermanentRefusalIsAskedAgainOnceADay) {
  RegisterOnPreferred();
  for (const Error& refusal : {AppError::Auth(Err::Auth::ClientUpgradeRequired, "426"),
                               AppError::Auth(Err::Auth::RelayUpgradeRequired, "relay_upgrade_required"),
                               AppError::Auth(Err::Auth::WrongAudience, "wrong_audience")}) {
    workflow_->MakeDue(kOther);
    client_->refusal = refusal;
    const int before = client_->finish_calls;
    workflow_->RunDue(kNow);
    EXPECT_EQ(client_->finish_calls, before + 1);
    workflow_->RunDue(kNow + RelayRegistrationsWorkflow::kMaxRetryMs);
    workflow_->RunDue(kNow + RelayRegistrationsWorkflow::kRefusedRetryMs - 1);
    EXPECT_EQ(client_->finish_calls, before + 1) << refusal.message << ": not before a day";
    workflow_->RunDue(kNow + RelayRegistrationsWorkflow::kRefusedRetryMs);
    EXPECT_EQ(client_->finish_calls, before + 2) << refusal.message;
  }
  client_->refusal.reset();
  workflow_->MakeDue(kOther);
  workflow_->RunDue(kNow);
  EXPECT_TRUE(workflow_->Registered(kOther));
}

// U11 / X12: a relay older than this app, and a relay that did not take our card, each say so in their own code.
TEST_F(RelayRegistrationsWorkflowTest, ARelayOlderThanThisAppOrRefusingOurCardSaysWhich) {
  RegisterOnPreferred();
  std::vector<RelayRegistrationRecord> records;
  workflow_->SetOnResult(
      [&records](const std::string&, const RelayRegistrationRecord& record) { records.push_back(record); });
  client_->refusal = AppError::Auth(Err::Auth::RelayUpgradeRequired, "relay_upgrade_required");
  workflow_->RunDue(kNow);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].reason, kRegistrationRelayUpgrade);
  workflow_->MakeDue(kOther);
  client_->refusal = AppError::Auth(Err::Auth::AccountCardRefused, "invalid_account_card");
  workflow_->RunDue(kNow);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[1].reason, kRegistrationCard);
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

// T3: a Remove the services did not follow leaves the relay ours: it is registered on again, its push with it.
TEST_F(RelayRegistrationsWorkflowTest, ARelayRejoinedAfterARevokeIsRegisteredOnAgain) {
  RegisterOnPreferred();
  workflow_->RunDue(kNow);
  ASSERT_TRUE(static_cast<bool>(workflow_->Revoke(kOther)));
  workflow_->SetRelays({{kOther, client_}}); // the old list, kept: still leaving
  EXPECT_TRUE(workflow_->Due(kNow).empty());
  workflow_->Rejoin(kOther);
  EXPECT_EQ(workflow_->Due(kNow), std::vector<std::string>({kOther}));
  registered_on_.clear();
  workflow_->RunDue(kNow);
  EXPECT_TRUE(workflow_->Registered(kOther));
  EXPECT_EQ(registered_on_, std::vector<std::string>({kOther}));
}

TEST_F(RelayRegistrationsWorkflowTest, ARelayRemovedWhileItRegistersIsRevokedAfterAndItsAnswerDropped) {
  // My relays' Remove while the timer's run registers there for a newer card: the answer is dropped (no push bound
  // there) and the revoke reaches the relay after the registration, so ours does not outlive the removal.
  RegisterOnPreferred();
  workflow_->RunDue(kNow);
  ASSERT_TRUE(workflow_->Registered(kOther));
  card_ = CardIssuedAt(2000);
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  client_->on_finish = [&entered, released]() {
    entered.set_value();
    released.wait();
  };
  std::thread run([this]() { workflow_->RunDue(kNow + 1000); });
  entered.get_future().wait();
  auto revoke = std::async(std::launch::async, [this]() { return workflow_->Revoke(kOther); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (workflow_->Registered(kOther) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_FALSE(workflow_->Registered(kOther)) << "leaving from the call on";
  release.set_value();
  run.join();
  ASSERT_TRUE(static_cast<bool>(revoke.get()));
  EXPECT_EQ(client_->events, (std::vector<std::string>{"finish", "finish", "revoke"}));
  EXPECT_EQ(registered_on_, std::vector<std::string>({kOther})) << "only the first registration binds push there";
  EXPECT_FALSE(workflow_->Registered(kOther));
  EXPECT_TRUE(workflow_->Due(kNow + 2000).empty()) << "a relay leaving is not registered on again";
}

TEST_F(RelayRegistrationsWorkflowTest, CloseWaitsForARunInProgressAndNothingRunsAfter) {
  // The profile closes (Shutdown, a reset) while the timer's run registers: Close returns once that run ended, and a
  // run queued before it then does nothing — the identity it would read is about to go.
  RegisterOnPreferred();
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  client_->on_finish = [&entered, released]() {
    entered.set_value();
    released.wait();
  };
  std::thread run([this]() { workflow_->RunDue(kNow); });
  entered.get_future().wait();
  auto closed = std::async(std::launch::async, [this]() { workflow_->Close(); });
  EXPECT_EQ(closed.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout) << "Close waits for the run";
  release.set_value();
  run.join();
  closed.get();
  client_->on_finish = nullptr;
  card_ = CardIssuedAt(2000);
  EXPECT_TRUE(workflow_->Due(kNow).empty());
  workflow_->RunDue(kNow + 1000);
  workflow_->RevokeAll();
  EXPECT_FALSE(static_cast<bool>(workflow_->Revoke(kOther)));
  EXPECT_EQ(client_->events, (std::vector<std::string>{"finish"})) << "nothing is asked once closed";
}

} // namespace
} // namespace pbr
