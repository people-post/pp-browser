#include "foundation/crypto/Sha256Hex.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(Sha256HexTest, EmptyInput) {
  EXPECT_EQ(Sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256HexTest, Abc) {
  EXPECT_EQ(Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

} // namespace
} // namespace pbr
