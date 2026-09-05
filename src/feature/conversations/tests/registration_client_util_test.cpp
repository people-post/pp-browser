#include "feature/conversations/RegistrationClientUtil.h"
#include "common/directory/IdentityTypes.h"
#include "common/Utilities.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace {

using namespace pbr;

TEST(RegistrationClientUtilTest, ClassifyUnregistered) {
  LocalIdentity identity;
  EXPECT_EQ(ClassifyRegistration(identity), RegistrationStatus::Unregistered);
  EXPECT_FALSE(ShouldRenewRegistration(identity));
  EXPECT_EQ(RegistrationStatusLabel(RegistrationStatus::Unregistered), "not registered");
  EXPECT_EQ(RegistrationActionLabel(RegistrationStatus::Unregistered), "Register on network");
}

TEST(RegistrationClientUtilTest, ClassifyActiveExpiringExpired) {
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

TEST(RegistrationClientUtilTest, ApplyRegistrationResultPersistsFields) {
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

TEST(RegistrationClientUtilTest, ApplyRegistrationResultIgnoresMissingFloor) {
  LocalIdentity identity;
  identity.initiation_floor = 7;
  RegistrationResult result{.success = true, .relay_user_id = "relay:x"};
  ApplyRegistrationResult(identity, result);
  EXPECT_EQ(identity.initiation_floor, 7);
}

TEST(RegistrationClientUtilTest, MarkRegistrationExpired) {
  LocalIdentity identity;
  identity.registered = true;
  identity.registration_expires_at = "2099-01-01T00:00:00.000Z";
  MarkRegistrationExpired(identity);
  EXPECT_EQ(ClassifyRegistration(identity, util::NowUnixMs()), RegistrationStatus::Expired);
}

} // namespace
