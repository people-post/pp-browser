#include "lib/amp/L2/SessionCrypto.h"

#include <gtest/gtest.h>

namespace pbr::amp {
namespace {

TEST(SessionCryptoTest, SealOpenRoundTrip) {
  ByteVector key(32, 0x55);
  const std::vector<uint8_t> plain = {'h', 'e', 'l', 'l', 'o'};
  auto sealed = SessionCrypto::Seal(key, 1, 7, 3, Direction::InitiatorToResponder, plain);
  ASSERT_TRUE(static_cast<bool>(sealed));
  auto opened = SessionCrypto::Open(key, 1, 7, 3, Direction::InitiatorToResponder, *sealed);
  ASSERT_TRUE(static_cast<bool>(opened));
  EXPECT_EQ(*opened, plain);
}

TEST(SessionCryptoTest, WrongAadFailsOpen) {
  ByteVector key(32, 0x66);
  const std::vector<uint8_t> plain = {'x'};
  auto sealed = SessionCrypto::Seal(key, 1, 1, 1, Direction::InitiatorToResponder, plain);
  ASSERT_TRUE(static_cast<bool>(sealed));
  auto opened = SessionCrypto::Open(key, 1, 1, 2, Direction::InitiatorToResponder, *sealed);
  EXPECT_FALSE(static_cast<bool>(opened));
}

} // namespace
} // namespace pbr::amp
