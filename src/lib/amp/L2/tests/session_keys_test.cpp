#include "lib/amp/L2/SessionKeys.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

TEST(SessionKeysTest, TranscriptHashDeterministic) {
  std::vector<ByteVector> parts = {ByteVector{'a', 'b'}, ByteVector{'c'}};
  auto h1 = SessionKeys::TranscriptHash(parts);
  auto h2 = SessionKeys::TranscriptHash(parts);
  ASSERT_TRUE(static_cast<bool>(h1));
  ASSERT_TRUE(static_cast<bool>(h2));
  EXPECT_EQ(*h1, *h2);
  EXPECT_EQ(h1->size(), 32u);
}

TEST(SessionKeysTest, DeriveDirectionalKeysDiffer) {
  ByteVector master(32, 0x11);
  ByteVector transcript(32, 0x22);
  auto initiator = SessionKeys::Derive(master, transcript, true, 1);
  auto responder = SessionKeys::Derive(master, transcript, false, 1);
  ASSERT_TRUE(static_cast<bool>(initiator));
  ASSERT_TRUE(static_cast<bool>(responder));
  EXPECT_EQ(initiator->k_assoc, responder->k_assoc);
  EXPECT_NE(initiator->k_send, responder->k_send);
  EXPECT_EQ(initiator->k_send, responder->k_recv);
  EXPECT_EQ(initiator->k_recv, responder->k_send);
}

TEST(SessionKeysTest, RekeyChangesSendRecv) {
  ByteVector master(32, 0x33);
  ByteVector transcript(32, 0x44);
  auto epoch1 = SessionKeys::Derive(master, transcript, true, 1);
  auto epoch2 = SessionKeys::Derive(master, transcript, true, 2);
  ASSERT_TRUE(static_cast<bool>(epoch1));
  ASSERT_TRUE(static_cast<bool>(epoch2));
  EXPECT_EQ(epoch1->k_assoc, epoch2->k_assoc);
  EXPECT_NE(epoch1->k_send, epoch2->k_send);
}

} // namespace
} // namespace pp::amp
