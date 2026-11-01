#include "lib/people/PeopleLookupFailure.h"

#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(PeopleLookupFailureTest, NotFoundTexts) {
  EXPECT_EQ(ClassifyPeopleLookupError(Error("Account not found")), PeopleLookupFailure::NotFound);
  EXPECT_EQ(ClassifyPeopleLookupError(Error("Relay user not found")), PeopleLookupFailure::NotFound);
}

TEST(PeopleLookupFailureTest, ServerErrorsAreUnreachable) {
  EXPECT_EQ(ClassifyPeopleLookupError(Error("Directory search failed with status 503")),
            PeopleLookupFailure::DirectoryUnreachable);
  EXPECT_EQ(ClassifyPeopleLookupError(Error("Directory lookup failed with status 500")),
            PeopleLookupFailure::DirectoryUnreachable);
}

TEST(PeopleLookupFailureTest, TransportErrorsAreUnreachable) {
  EXPECT_EQ(ClassifyPeopleLookupError(
                AppError::Network(Err::Network::ConnectFailed, "curl failed: Couldn't connect to server")),
            PeopleLookupFailure::DirectoryUnreachable);
  EXPECT_EQ(ClassifyPeopleLookupError(AppError::Network(Err::Network::Timeout, "curl failed: Timeout was reached")),
            PeopleLookupFailure::DirectoryUnreachable);
  EXPECT_EQ(ClassifyPeopleLookupError(
                AppError::Network(Err::Network::DnsFailed, "curl failed: Could not resolve host")),
            PeopleLookupFailure::DirectoryUnreachable);
}

TEST(PeopleLookupFailureTest, EverythingElseIsOther) {
  EXPECT_EQ(ClassifyPeopleLookupError(Error("Directory lookup failed with status 403")), PeopleLookupFailure::Other);
  EXPECT_EQ(ClassifyPeopleLookupError(Error("Directory lookup failed with status 429")), PeopleLookupFailure::Other);
  EXPECT_EQ(ClassifyPeopleLookupError(Error("Directory base_url not configured")), PeopleLookupFailure::Other);
  EXPECT_EQ(ClassifyPeopleLookupError(Error("")), PeopleLookupFailure::Other);
}

} // namespace
} // namespace pbr
