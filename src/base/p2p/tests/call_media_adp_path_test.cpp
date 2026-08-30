#include "base/adp/Clock.h"
#include "base/adp/MemoryDatagramIo.h"
#include "base/p2p/CallMediaAdpKey.h"
#include "base/p2p/CallMediaAdpPath.h"
#include "base/p2p/CallMediaFrameCrypto.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <atomic>
#include <string>
#include <vector>

namespace pbr {
namespace {

ByteVector FakeKey() { return ByteVector(32, 0x41); }

class CallMediaAdpPathTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

TEST_F(CallMediaAdpPathTest, AssocKeyStableAndEpochBound) {
  const std::string call_id = "call:adp-hkdf";
  auto a = DeriveCallMediaAdpAssocKey(FakeKey(), call_id, 1);
  auto b = DeriveCallMediaAdpAssocKey(FakeKey(), call_id, 1);
  auto c = DeriveCallMediaAdpAssocKey(FakeKey(), call_id, 2);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  ASSERT_TRUE(c);
  EXPECT_EQ(a->bytes, b->bytes);
  EXPECT_NE(a->bytes, c->bytes);
}

TEST_F(CallMediaAdpPathTest, AssocHexRoundTrip) {
  const adp::AssocId id = MintCallMediaAdpAssocId();
  const std::string hex = AssocIdToHex(id);
  EXPECT_EQ(hex.size(), 32u);
  auto back = AssocIdFromHex(hex);
  ASSERT_TRUE(back);
  EXPECT_EQ(back->bytes, id.bytes);
  EXPECT_FALSE(AssocIdFromHex("dead"));
  EXPECT_FALSE(AssocIdFromHex(std::string(30, 'a')));
}

TEST_F(CallMediaAdpPathTest, MemoryIoOpusRoundTrip) {
  auto clock = std::make_shared<adp::VirtualClock>(1'000'000);
  auto hub = adp::MemoryDatagramIo::MakeHub();
  const auto addr_a = adp::IpEndpoint::V4(10, 0, 0, 1, 4001);
  const auto addr_b = adp::IpEndpoint::V4(10, 0, 0, 2, 4002);
  auto io_a = std::make_shared<adp::MemoryDatagramIo>(hub, addr_a);
  auto io_b = std::make_shared<adp::MemoryDatagramIo>(hub, addr_b);

  CallMediaAdpPath offerer;
  CallMediaAdpPath answerer;
  offerer.SetIoForTest(io_a, clock);
  answerer.SetIoForTest(io_b, clock);

  auto o_offer = offerer.BindLocal(/*offerer_mints_assoc=*/true);
  ASSERT_TRUE(o_offer) << o_offer.error().message;
  EXPECT_EQ(o_offer->port, 4001);
  EXPECT_EQ(o_offer->ipv4, "10.0.0.1");

  auto a_offer = answerer.BindLocal(/*offerer_mints_assoc=*/false);
  ASSERT_TRUE(a_offer) << a_offer.error().message;
  answerer.SetLocalAssoc(o_offer->assoc);

  const std::string call_id = "call:adp-opus";
  const ByteVector key = FakeKey();
  std::atomic<int> got{0};
  std::vector<uint8_t> last;

  CallMediaAdpHelloOffer remote_for_a = *o_offer;
  ASSERT_TRUE(answerer.Activate(key, call_id, 1, remote_for_a,
                                [&](uint8_t ch, const std::vector<uint8_t>& payload) {
                                  EXPECT_EQ(ch, kCallMediaChannelAudio);
                                  last = payload;
                                  got.fetch_add(1);
                                }));

  CallMediaAdpHelloOffer remote_for_o = answerer.LocalOffer();
  remote_for_o.assoc = o_offer->assoc;
  ASSERT_TRUE(offerer.Activate(key, call_id, 1, remote_for_o, nullptr));

  const std::vector<uint8_t> opus = {0x01, 0x02, 0x03, 0x04, 0xaa};
  ASSERT_TRUE(offerer.SendOpus(opus, /*seq=*/7, /*mark=*/0)) << "send failed";
  answerer.Pump();
  ASSERT_EQ(got.load(), 1);
  EXPECT_EQ(last, opus);
}

TEST_F(CallMediaAdpPathTest, ActivateRequiresRemoteOffer) {
  CallMediaAdpPath path;
  auto clock = std::make_shared<adp::VirtualClock>(1);
  auto hub = adp::MemoryDatagramIo::MakeHub();
  auto io = std::make_shared<adp::MemoryDatagramIo>(hub, adp::IpEndpoint::V4(127, 0, 0, 1, 9));
  path.SetIoForTest(io, clock);
  ASSERT_TRUE(path.BindLocal(true));
  CallMediaAdpHelloOffer empty;
  EXPECT_FALSE(path.Activate(FakeKey(), "c", 1, empty, nullptr));
}

} // namespace
} // namespace pbr
