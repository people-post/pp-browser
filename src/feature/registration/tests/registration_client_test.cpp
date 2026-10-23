#include "feature/registration/RegistrationClient.h"
#include "common/directory/IdentityTypes.h"
#include "common/Utilities.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/IdentityStore.h"
#include "foundation/crypto/CryptoConstants.h"

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

} // namespace
