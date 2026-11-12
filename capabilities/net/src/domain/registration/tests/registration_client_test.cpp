#include "domain/registration/RegistrationClient.h"
#include "domain/registration/BriefLlmKeyRenewal.h"
#include "common/directory/IdentityTypes.h"
#include "common/Utilities.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/IdentityStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/KemKeyBinding.h"

#include <filesystem>
#include <thread>

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace {

using namespace pbr;

TEST(RegistrationClientTest, ClassifyUnregistered) {
  LocalIdentity identity;
  EXPECT_EQ(ClassifyRegistration(identity), RegistrationStatus::Unregistered);
  EXPECT_FALSE(ShouldRenewRegistration(identity));
  EXPECT_EQ(RegistrationStatusLabel(RegistrationStatus::Unregistered), "not registered");
  EXPECT_EQ(RegistrationActionLabel(RegistrationStatus::Unregistered), "Register on network");
}

TEST(RegistrationClientTest, ClassifyActiveExpiringExpired) {
  LocalIdentity identity;
  identity.registered = true;
  identity.registration_expires_at = "2099-01-01T00:00:00.000Z";
  EXPECT_EQ(ClassifyRegistration(identity, 1'700'000'000'000), RegistrationStatus::Active);
  EXPECT_FALSE(ShouldRenewRegistration(identity, 1'700'000'000'000));

  identity.registration_expires_at = "1970-01-15T00:00:00.000Z";
  EXPECT_EQ(ClassifyRegistration(identity, 1'700'000'000'000), RegistrationStatus::Expired);
  EXPECT_TRUE(ShouldRenewRegistration(identity, 1'700'000'000'000));
  EXPECT_EQ(RegistrationActionLabel(RegistrationStatus::Expired), "Renew registration");

  // Within 14 days of expiry relative to a fixed "now".
  identity.registration_expires_at = "2024-01-10T00:00:00.000Z";
  const int64_t now = 1'704'067'200'000; // 2024-01-01T00:00:00Z
  EXPECT_EQ(ClassifyRegistration(identity, now), RegistrationStatus::ExpiringSoon);
  EXPECT_TRUE(ShouldRenewRegistration(identity, now));
  EXPECT_EQ(RegistrationStatusLabel(RegistrationStatus::ExpiringSoon), "expiring soon");
}

TEST(RegistrationClientTest, ApplyRegistrationResultPersistsFields) {
  LocalIdentity identity;
  identity.brief_llm_guest_api_key = "brf_guest_old";
  RegistrationResult result{.success = true,
                            .relay_user_id = "relay:abc",
                            .message = "ok",
                            .llm_api_key = "brf_llm_new",
                            .expires_at = "2099-12-31T23:59:59.000Z",
                            .initiation_floor = 99,
                            .initiation_floor_present = true};
  ApplyRegistrationResult(identity, result);
  EXPECT_TRUE(identity.registered);
  EXPECT_EQ(identity.relay_user_id, "relay:abc");
  EXPECT_EQ(identity.brief_llm_api_key, "brf_llm_new");
  EXPECT_TRUE(identity.brief_llm_guest_api_key.empty());
  EXPECT_EQ(identity.registration_expires_at, "2099-12-31T23:59:59.000Z");
  EXPECT_EQ(identity.initiation_floor, 99);
}

TEST(RegistrationClientTest, ApplyRegistrationResultIgnoresMissingFloor) {
  LocalIdentity identity;
  identity.initiation_floor = 7;
  RegistrationResult result{.success = true, .relay_user_id = "relay:x"};
  ApplyRegistrationResult(identity, result);
  EXPECT_EQ(identity.initiation_floor, 7);
}

TEST(RegistrationClientTest, NormalizeRegistrationBackendMatchesTheBackendProfileRule) {
  EXPECT_EQ(NormalizeRegistrationBackend("  HTTPS://Relay.Example.Test/ "), "https://relay.example.test");
  EXPECT_EQ(NormalizeRegistrationBackend("https://relay.example.test/Base/"), "https://relay.example.test/Base");
  EXPECT_EQ(NormalizeRegistrationBackend(""), "");
}

TEST(RegistrationClientTest, ARegistrationAtAnotherOrUnrecordedBackendNeedsRenewal) {
  LocalIdentity identity;
  identity.registered = true;
  identity.registration_expires_at = "2099-01-01T00:00:00.000Z";
  const int64_t now = 1'700'000'000'000;
  const std::string current = "https://new.example.test/";

  EXPECT_EQ(ClassifyRegistration(identity, now, current), RegistrationStatus::WrongBackend)
      << "recorded by a build that did not say where";
  EXPECT_TRUE(ShouldRenewRegistration(identity, now, current));
  EXPECT_EQ(RegistrationActionLabel(RegistrationStatus::WrongBackend), "Renew registration");

  identity.registered_backend_url = "https://old.example.test";
  EXPECT_EQ(ClassifyRegistration(identity, now, current), RegistrationStatus::WrongBackend);

  identity.registered_backend_url = "https://new.example.test";
  EXPECT_EQ(ClassifyRegistration(identity, now, current), RegistrationStatus::Active)
      << "same backend, trailing / aside";
  EXPECT_FALSE(ShouldRenewRegistration(identity, now, current));
  EXPECT_TRUE(RegisteredAtBackend(identity, ""));

  LocalIdentity unregistered;
  EXPECT_EQ(ClassifyRegistration(unregistered, now, current), RegistrationStatus::Unregistered);
}

TEST(RegistrationClientTest, ApplyRegistrationResultRecordsTheBackend) {
  LocalIdentity identity;
  identity.registered_backend_url = "https://old.example.test";
  ApplyRegistrationResult(identity, RegistrationResult{.success = true, .relay_user_id = "relay:x"});
  EXPECT_EQ(identity.registered_backend_url, "https://old.example.test") << "a result without one keeps the record";

  RegistrationResult result{.success = true, .relay_user_id = "relay:x"};
  result.registered_backend_url = "https://new.example.test";
  ApplyRegistrationResult(identity, result);
  EXPECT_EQ(identity.registered_backend_url, "https://new.example.test");
}

TEST(RegistrationClientTest, MarkRegistrationExpired) {
  LocalIdentity identity;
  identity.registered = true;
  identity.registration_expires_at = "2099-01-01T00:00:00.000Z";
  MarkRegistrationExpired(identity);
  EXPECT_EQ(ClassifyRegistration(identity, util::NowUnixMs()), RegistrationStatus::Expired);
}

// #328: records the revoke call; everything else is the mock's behaviour.
class RecordingRegistrationClient : public MockRegistrationClient {
public:
  Roe<void> RevokeRegistration(const std::string& relay_user_id, const std::string& signature,
                               const int64_t timestamp) override {
    ++revoke_calls;
    last_relay_user_id = relay_user_id;
    last_signature = signature;
    last_timestamp = timestamp;
    return {};
  }
  int revoke_calls = 0;
  std::string last_relay_user_id;
  std::string last_signature;
  int64_t last_timestamp = 0;
};

class RevokeRegistrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_revoke_registration_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    identity_ = std::make_unique<IdentityStore>(dir_.string(), "test-profile");
    ASSERT_TRUE(static_cast<bool>(identity_->SetDek(ByteVector(kDataEncryptionKeySize, 7))));
    auto created = identity_->LoadOrCreate();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  }
  void TearDown() override {
    identity_.reset();
    std::filesystem::remove_all(dir_);
  }
  std::filesystem::path dir_;
  std::unique_ptr<IdentityStore> identity_;
  RecordingRegistrationClient client_;
};

TEST_F(RevokeRegistrationTest, AutoRenewReRegistersAtTheCurrentBackendAndRecordsIt) {
  auto identity = identity_->Get();
  ASSERT_TRUE(static_cast<bool>(identity));
  identity->registered = true;
  identity->relay_user_id = "relay:u1";
  identity->registration_expires_at = "2099-01-01T00:00:00.000Z";
  identity->kem_binding_published = true;
  identity->registered_backend_url = "https://old.example.test";
  ASSERT_TRUE(static_cast<bool>(identity_->Update(*identity)));
  client_.SetBaseUrl("https://new.example.test/");

  auto off = MaybeAutoRenewRegistration(client_, *identity_, /*auto_renew_enabled=*/false);
  ASSERT_TRUE(static_cast<bool>(off)) << off.error().message;
  EXPECT_FALSE(*off) << "with auto-renew off nothing happens";

  auto renewed = MaybeAutoRenewRegistration(client_, *identity_, /*auto_renew_enabled=*/true);
  ASSERT_TRUE(static_cast<bool>(renewed)) << renewed.error().message;
  EXPECT_TRUE(*renewed);
  auto stored = identity_->Get();
  ASSERT_TRUE(static_cast<bool>(stored));
  EXPECT_EQ(stored->registered_backend_url, "https://new.example.test");

  auto again = MaybeAutoRenewRegistration(client_, *identity_, /*auto_renew_enabled=*/true);
  ASSERT_TRUE(static_cast<bool>(again)) << again.error().message;
  EXPECT_FALSE(*again) << "registered at this backend now: nothing is due";
}

TEST_F(RevokeRegistrationTest, UnregisteredIdentityIsANoOp) {
  auto result = RevokeRegistrationWithIdentity(client_, *identity_);
  EXPECT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(client_.revoke_calls, 0);
}

TEST_F(RevokeRegistrationTest, RegisteredIdentitySignsAndCallsTheRelay) {
  auto identity = identity_->Get();
  ASSERT_TRUE(static_cast<bool>(identity));
  identity->registered = true;
  identity->relay_user_id = "relay:u1";
  ASSERT_TRUE(static_cast<bool>(identity_->Update(*identity)));

  auto result = RevokeRegistrationWithIdentity(client_, *identity_);
  ASSERT_TRUE(static_cast<bool>(result)) << result.error().message;
  EXPECT_EQ(client_.revoke_calls, 1);
  EXPECT_EQ(client_.last_relay_user_id, "relay:u1");
  EXPECT_FALSE(client_.last_signature.empty());
  EXPECT_GT(client_.last_timestamp, 0);
  // The identity itself is untouched: the caller wipes it right after.
  auto after = identity_->Get();
  ASSERT_TRUE(static_cast<bool>(after));
  EXPECT_TRUE(after->registered);
}

// Records what register/finish sent; the relay stores the binding when `relay_stores_binding`.
class BindingRecordingClient : public MockRegistrationClient {
public:
  Roe<RegistrationResult> FinishRegistration(const std::string& challenge, const std::string& public_key_b64,
                                             const std::string& nickname, const std::string& signature,
                                             int64_t timestamp, const std::string& signature_alg,
                                             const std::string& kem_public_key_b64,
                                             const std::string& kem_binding_signature_b64, const std::string& peer_id,
                                             const std::vector<std::string>& multiaddrs, int64_t initiation_floor,
                                             const RegistrationPublishOpts& publish) override {
    ++finish_calls;
    last_kem_public_key_b64 = kem_public_key_b64;
    last_binding = kem_binding_signature_b64;
    auto result = MockRegistrationClient::FinishRegistration(
        challenge, public_key_b64, nickname, signature, timestamp, signature_alg, kem_public_key_b64,
        kem_binding_signature_b64, peer_id, multiaddrs, initiation_floor, publish);
    if (result) {
      result->kem_bound = relay_stores_binding;
    }
    return result;
  }
  bool relay_stores_binding = true;
  int finish_calls = 0;
  std::string last_kem_public_key_b64;
  std::string last_binding;
};

class KemBindingPublishTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_kem_binding_publish_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    identity_ = std::make_unique<IdentityStore>(dir_.string(), "test-profile");
    ASSERT_TRUE(static_cast<bool>(identity_->SetDek(ByteVector(kDataEncryptionKeySize, 7))));
    auto created = identity_->LoadOrCreate();
    ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
    // Registered before bindings existed, and far from expiry.
    LocalIdentity identity = *created;
    identity.registered = true;
    identity.relay_user_id = "relay:u1";
    identity.registration_expires_at = "2099-01-01T00:00:00.000Z";
    ASSERT_TRUE(static_cast<bool>(identity_->Update(identity)));
  }
  void TearDown() override {
    identity_.reset();
    std::filesystem::remove_all(dir_);
  }
  std::filesystem::path dir_;
  std::unique_ptr<IdentityStore> identity_;
  BindingRecordingClient client_;
};

TEST_F(KemBindingPublishTest, AnOlderRegistrationPublishesItsBindingOnceEvenWithoutAutoRenew) {
  auto renewed = MaybeAutoRenewRegistration(client_, *identity_, /*auto_renew_enabled=*/false);
  ASSERT_TRUE(static_cast<bool>(renewed)) << renewed.error().message;
  EXPECT_TRUE(*renewed);
  ASSERT_EQ(client_.finish_calls, 1);

  const LocalIdentity after = identity_->Get().value();
  EXPECT_TRUE(after.kem_binding_published);
  EXPECT_EQ(client_.last_kem_public_key_b64, after.kem_public_key_b64);
  EXPECT_TRUE(VerifyKemBinding(after.account_id, after.account_signing_public_key_b64, after.kem_public_key_b64,
                               client_.last_binding))
      << "register/finish carries the account's own binding";

  renewed = MaybeAutoRenewRegistration(client_, *identity_, /*auto_renew_enabled=*/false);
  ASSERT_TRUE(static_cast<bool>(renewed));
  EXPECT_FALSE(*renewed);
  EXPECT_EQ(client_.finish_calls, 1) << "published: no further registration";
}

TEST_F(KemBindingPublishTest, ARelayThatDidNotStoreItLeavesItToPublishAgain) {
  client_.relay_stores_binding = false;
  ASSERT_TRUE(static_cast<bool>(MaybeAutoRenewRegistration(client_, *identity_, false)));
  EXPECT_FALSE(identity_->Get().value().kem_binding_published);
  EXPECT_TRUE(ShouldPublishKemBinding(identity_->Get().value()));
}

// DEV/PLAN-unlock-gate-early-dismiss.md A.2: the renewal in two halves (network off the UI thread, store back on it).
// RenewNetworkOnly then PersistRenewal stores exactly what FinishAndPersistRegistration did; the network half stores
// nothing; the store half reads the identity again, so a field written in between is kept.
void ExpectSameRegistrationFields(const LocalIdentity& a, const LocalIdentity& b) {
  EXPECT_EQ(a.registered, b.registered);
  EXPECT_EQ(a.relay_user_id, b.relay_user_id);
  EXPECT_EQ(a.brief_llm_api_key.Reveal(), b.brief_llm_api_key.Reveal());
  EXPECT_EQ(a.brief_llm_guest_api_key.Reveal(), b.brief_llm_guest_api_key.Reveal());
  EXPECT_EQ(a.registration_expires_at, b.registration_expires_at);
  EXPECT_EQ(a.initiation_floor, b.initiation_floor);
  EXPECT_EQ(a.kem_binding_published, b.kem_binding_published);
  EXPECT_EQ(a.nickname, b.nickname);
  EXPECT_EQ(a.account_id, b.account_id);
  EXPECT_EQ(a.peer_id, b.peer_id);
  EXPECT_EQ(a.kem_public_key_b64, b.kem_public_key_b64);
}

TEST_F(KemBindingPublishTest, TheSplitHalvesStoreWhatFinishAndPersistStored) {
  const LocalIdentity initial = identity_->Get().value();

  auto old_applied = FinishAndPersistRegistration(client_, *identity_, initial.nickname);
  ASSERT_TRUE(static_cast<bool>(old_applied)) << old_applied.error().message;
  const LocalIdentity after_old = identity_->Get().value();
  ExpectSameRegistrationFields(*old_applied, after_old);

  ASSERT_TRUE(static_cast<bool>(identity_->Update(initial)));
  auto result = RenewNetworkOnly(client_, *identity_, initial.nickname);
  ASSERT_TRUE(static_cast<bool>(result)) << result.error().message;
  auto new_applied = PersistRenewal(*identity_, *result, initial.account_id);
  ASSERT_TRUE(static_cast<bool>(new_applied)) << new_applied.error().message;
  const LocalIdentity after_new = identity_->Get().value();

  ExpectSameRegistrationFields(after_old, after_new);
  ExpectSameRegistrationFields(*new_applied, after_new);
  EXPECT_EQ(client_.finish_calls, 2);
  EXPECT_TRUE(after_new.kem_binding_published);
  EXPECT_EQ(after_new.brief_llm_api_key.Reveal(), "brf_llm_mock_key");
}

TEST_F(KemBindingPublishTest, RenewNetworkOnlyStoresNothing) {
  const LocalIdentity before = identity_->Get().value();
  auto result = RenewNetworkOnly(client_, *identity_, before.nickname);
  ASSERT_TRUE(static_cast<bool>(result)) << result.error().message;
  EXPECT_EQ(client_.finish_calls, 1);
  ExpectSameRegistrationFields(before, identity_->Get().value());
}

TEST_F(KemBindingPublishTest, PersistRenewalReadsTheIdentityAgain) {
  auto result = RenewNetworkOnly(client_, *identity_, identity_->Get().value().nickname);
  ASSERT_TRUE(static_cast<bool>(result));
  LocalIdentity meanwhile = identity_->Get().value();
  meanwhile.nickname = "renamed-meanwhile"; // written while the network half was out
  ASSERT_TRUE(static_cast<bool>(identity_->Update(meanwhile)));
  ASSERT_TRUE(static_cast<bool>(PersistRenewal(*identity_, *result, meanwhile.account_id)));
  const LocalIdentity after = identity_->Get().value();
  EXPECT_EQ(after.nickname, "renamed-meanwhile") << "only the registration fields are the renewal's";
  EXPECT_TRUE(after.kem_binding_published);
}

TEST_F(KemBindingPublishTest, MaybeAutoRenewNetworkOnlyFollowsTheSameRule) {
  auto due = MaybeAutoRenewNetworkOnly(client_, *identity_, /*auto_renew_enabled=*/false);
  ASSERT_TRUE(static_cast<bool>(due)) << due.error().message;
  ASSERT_TRUE(due->has_value()) << "an unpublished KEM binding is due even without auto-renew";
  EXPECT_FALSE(identity_->Get().value().kem_binding_published) << "nothing stored by the network half";
  ASSERT_TRUE(static_cast<bool>(PersistRenewal(*identity_, **due, identity_->Get().value().account_id)));

  auto not_due = MaybeAutoRenewNetworkOnly(client_, *identity_, /*auto_renew_enabled=*/true);
  ASSERT_TRUE(static_cast<bool>(not_due));
  EXPECT_FALSE(not_due->has_value()) << "published and far from expiry: nothing due";
  EXPECT_EQ(client_.finish_calls, 1);
}

// A renewal stored on the UI thread and a guest-key write off it (the 401 renewal, the diagnostics Bearer) at the
// same time: both are one modify under the store's lock, so neither overwrites the other's field.
TEST_F(KemBindingPublishTest, ConcurrentRenewalAndGuestKeyWritesKeepBothFields) {
  const std::string account = identity_->Get().value().account_id;
  BriefLlmKeyStore keys;
  keys.get = [this]() -> std::optional<LocalIdentity> {
    auto got = identity_->Get();
    return got ? std::optional<LocalIdentity>(*got) : std::nullopt;
  };
  keys.modify = [this](const std::function<bool(LocalIdentity&)>& change) -> Roe<void> {
    if (auto changed = identity_->Modify(change); !changed) {
      return changed.error();
    }
    return {};
  };
  constexpr int kRounds = 60;
  std::thread renewals([&]() {
    for (int i = 0; i < kRounds; ++i) {
      RegistrationResult result{.success = true,
                                .relay_user_id = "relay:u1",
                                .expires_at = "2099-01-01T00:00:" + std::to_string(10 + i) + ".000Z",
                                .kem_bound = true};
      ASSERT_TRUE(static_cast<bool>(PersistRenewal(*identity_, result, account)));
    }
  });
  std::thread guest_keys([&]() {
    for (int i = 0; i < kRounds; ++i) {
      ASSERT_TRUE(static_cast<bool>(keys.modify([&](LocalIdentity& identity) {
        identity.brief_llm_guest_api_key = SecretText(("brf_guest_" + std::to_string(i)).c_str());
        return true;
      })));
    }
  });
  renewals.join();
  guest_keys.join();
  const LocalIdentity after = identity_->Get().value();
  EXPECT_EQ(after.registration_expires_at, "2099-01-01T00:00:" + std::to_string(10 + kRounds - 1) + ".000Z");
  EXPECT_EQ(after.brief_llm_guest_api_key.Reveal(), "brf_guest_" + std::to_string(kRounds - 1));
  EXPECT_TRUE(after.kem_binding_published);

  // The unlock's guest key arriving after a key is stored keeps that key.
  auto kept = StoreBriefGuestKeyIfNone(keys, SecretText("brf_guest_late"), account);
  ASSERT_TRUE(static_cast<bool>(kept));
  EXPECT_EQ(*kept, "brf_guest_" + std::to_string(kRounds - 1));
}

// Hardening: a renewal that ran for one account is not stored into another (the profile changed meanwhile).
TEST_F(KemBindingPublishTest, PersistRenewalRefusesAnotherAccount) {
  auto result = RenewNetworkOnly(client_, *identity_, identity_->Get().value().nickname);
  ASSERT_TRUE(static_cast<bool>(result));
  const LocalIdentity before = identity_->Get().value();
  EXPECT_FALSE(static_cast<bool>(PersistRenewal(*identity_, *result, "account:someone-else")));
  ExpectSameRegistrationFields(before, identity_->Get().value());
  EXPECT_FALSE(static_cast<bool>(PersistRenewal(*identity_, *result, ""))) << "no account: refused, not a pass";
  ExpectSameRegistrationFields(before, identity_->Get().value());
  EXPECT_TRUE(static_cast<bool>(PersistRenewal(*identity_, *result, before.account_id)));
  EXPECT_TRUE(identity_->Get().value().kem_binding_published);
}

} // namespace
