#include "lib/messaging/PeerRequestAuth.h"

#include "lib/people/PeerSigningKeyStore.h"
#include "lib/people/tests/TestAccount.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int64_t kNow = 1'761'235'200'000;

TEST(AccountCardRequestAuthTest, TheRequestersOwnKeyCardStandsInForALookup) {
  const test::TestAccount asker = test::TestAccount::Make();
  const test::TestAccount mallory = test::TestAccount::Make();
  AccountCardRequest request{.account_id = "account:someone", .requester_keys = asker.Card()};
  ASSERT_TRUE(PeerRequestAuth::Sign(request, asker.account_id, "QmAsker", kNow,
                                    [&](const std::vector<uint8_t>& bytes) { return asker.Sign(bytes); }));
  PeerSigningKeyStore none;
  PeerSigningKeyResolver no_keys{none};
  EXPECT_TRUE(PeerRequestAuth::Verify(request, "QmAsker", kNow, no_keys)) << "checked with the card it carries";
  EXPECT_FALSE(PeerRequestAuth::Verify(request, "QmOther", kNow, no_keys)) << "another link";
  AccountCardRequest swapped = request;
  swapped.requester_keys = mallory.Card();  // someone else's card: does not prove itself for the asker
  EXPECT_FALSE(PeerRequestAuth::Verify(swapped, "QmAsker", kNow, no_keys));
  AccountCardRequest retargeted = request;
  retargeted.account_id = "account:another";
  EXPECT_FALSE(PeerRequestAuth::Verify(retargeted, "QmAsker", kNow, no_keys)) << "the signature covers whose card";
}

} // namespace
} // namespace pbr
