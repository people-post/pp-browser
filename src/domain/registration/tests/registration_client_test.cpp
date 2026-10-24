#include "domain/registration/RegistrationClient.h"
#include "common/directory/IdentityTypes.h"
#include "common/Utilities.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/IdentityStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/KemKeyBinding.h"

#include <filesystem>

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
    auto result = MockRegistrationClient::FinishRegistration(challenge, public_key_b64, nickname, signature,
                                                             timestamp, signature_alg, kem_public_key_b64,
                                                             kem_binding_signature_b64, peer_id, multiaddrs,
                                                             initiation_floor, publish);
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

} // namespace
