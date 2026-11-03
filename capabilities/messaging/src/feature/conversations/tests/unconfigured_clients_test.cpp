// A hub whose backend clients were never built (no backend address: the UI demo app, a blank custom backend) hands out
// references that report an error instead of dereferencing null (DEV/PLAN-ui-demo-data.md).
#include "feature/conversations/ConversationsHub.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(UnconfiguredClientsTest, RegistrationReportsAnErrorInsteadOfCrashing) {
  ConversationsHub hub;
  IRegistrationClient& registration = hub.Registration();
  EXPECT_FALSE(registration.UpdateNickname("New name", "sig", 1, "relay:me"));
  EXPECT_FALSE(registration.StartRegistration("pk", "name"));
  EXPECT_FALSE(registration.FinishRegistration("challenge", "pk", "name", "sig", 1));
  EXPECT_FALSE(registration.RevokeRegistration("relay:me", "sig", 1));
  EXPECT_EQ(&registration, &hub.Registration());
}

TEST(UnconfiguredClientsTest, BlobReportsAnErrorInsteadOfCrashing) {
  ConversationsHub hub;
  IBlobClient& blob = hub.Blob();
  EXPECT_FALSE(blob.Presign("relay:me", "image/png", 10, BlobPurpose::File));
  EXPECT_FALSE(blob.FileStart("relay:me", 10));
  EXPECT_FALSE(blob.FileList("relay:me", ""));
  EXPECT_FALSE(blob.Delete("relay:me", "blob"));
  EXPECT_EQ(&blob, &hub.Blob());
}

} // namespace
} // namespace pbr
