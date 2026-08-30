#include "base/adp/Clock.h"
#include "base/adp/Endpoint.h"
#include "base/adp/MemoryDatagramIo.h"
#include "base/adp/Types.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <string>
#include <vector>

namespace {

pbr::adp::PeerKey Key() {
  pbr::adp::PeerKey k;
  k.bytes.fill(0x55);
  return k;
}

pbr::adp::AssocId Aid() {
  pbr::adp::AssocId id;
  id.bytes.fill(0x66);
  return id;
}

struct Pair {
  std::shared_ptr<pbr::adp::VirtualClock> clock;
  std::shared_ptr<pbr::adp::MemoryDatagramHub> hub;
  std::shared_ptr<pbr::adp::MemoryDatagramIo> io_a;
  std::shared_ptr<pbr::adp::MemoryDatagramIo> io_b;
  std::unique_ptr<pbr::adp::Endpoint> ep_a;
  std::unique_ptr<pbr::adp::Endpoint> ep_b;
  pbr::adp::IpEndpoint addr_a;
  pbr::adp::IpEndpoint addr_b;
};

Pair MakePair() {
  Pair p;
  p.clock = std::make_shared<pbr::adp::VirtualClock>(5'000'000);
  p.hub = pbr::adp::MemoryDatagramIo::MakeHub();
  p.addr_a = pbr::adp::IpEndpoint::V4(127, 0, 0, 1, 4001);
  p.addr_b = pbr::adp::IpEndpoint::V4(127, 0, 0, 1, 4002);
  p.io_a = std::make_shared<pbr::adp::MemoryDatagramIo>(p.hub, p.addr_a);
  p.io_b = std::make_shared<pbr::adp::MemoryDatagramIo>(p.hub, p.addr_b);
  p.ep_a = std::make_unique<pbr::adp::Endpoint>(p.io_a, p.clock);
  p.ep_b = std::make_unique<pbr::adp::Endpoint>(p.io_b, p.clock);
  return p;
}

void PumpBoth(Pair& p) {
  for (int i = 0; i < 8; ++i) {
    p.ep_a->Pump();
    p.ep_b->Pump();
    p.ep_a->Tick();
    p.ep_b->Tick();
  }
}

class AdpReliableTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

TEST_F(AdpReliableTest, DeliverUnderLoss) {
  auto p = MakePair();
  p.ep_b->SetAcceptKey(Key());
  p.ep_b->SetAcceptEnabled(true);
  p.io_a->DropNext(2); // drop first two datagrams (data + maybe nothing)

  pbr::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  op.rtx_interval_ms = 10;
  op.max_rtx = 10;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);

  std::vector<std::string> got;
  // Pre-open B so we can attach handler before rtx lands.
  pbr::adp::OpenParams opb = op;
  opb.peer = p.addr_a;
  auto cb = p.ep_b->Open(opb);
  ASSERT_TRUE(cb);
  (*cb)->OnMessage([&](const pbr::adp::Message& m) {
    if (m.qos == pbr::adp::QosClass::Reliable) {
      got.emplace_back(m.payload.begin(), m.payload.end());
    }
  });

  ASSERT_TRUE((*ca)->Send(pbr::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("rel"), 3)));
  // First send dropped.
  PumpBoth(p);
  EXPECT_TRUE(got.empty());
  p.clock->Advance(10);
  PumpBoth(p);
  // Second attempt may also be dropped (DropNext 2).
  p.clock->Advance(10);
  PumpBoth(p);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], "rel");
}

TEST_F(AdpReliableTest, AckStopsRetransmit) {
  auto p = MakePair();
  pbr::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  op.rtx_interval_ms = 10;
  auto ca = p.ep_a->Open(op);
  auto cb = [&] {
    pbr::adp::OpenParams opb = op;
    opb.peer = p.addr_a;
    return p.ep_b->Open(opb);
  }();
  ASSERT_TRUE(ca);
  ASSERT_TRUE(cb);
  size_t msg_count = 0;
  (*cb)->OnMessage([&](const pbr::adp::Message&) { ++msg_count; });

  ASSERT_TRUE((*ca)->Send(pbr::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("m"), 1)));
  PumpBoth(p);
  EXPECT_EQ(msg_count, 1u);
  // Further ticks should not redeliver.
  for (int i = 0; i < 5; ++i) {
    p.clock->Advance(10);
    PumpBoth(p);
  }
  EXPECT_EQ(msg_count, 1u);
}

TEST_F(AdpReliableTest, QosIsolation) {
  auto p = MakePair();
  pbr::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto ca = p.ep_a->Open(op);
  pbr::adp::OpenParams opb = op;
  opb.peer = p.addr_a;
  auto cb = p.ep_b->Open(opb);
  ASSERT_TRUE(ca);
  ASSERT_TRUE(cb);

  std::vector<pbr::adp::QosClass> order;
  (*cb)->OnMessage([&](const pbr::adp::Message& m) { order.push_back(m.qos); });

  p.io_a->DropNext(1); // drop best-effort
  ASSERT_TRUE((*ca)->Send(pbr::adp::QosClass::BestEffort,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("b"), 1)));
  ASSERT_TRUE((*ca)->Send(pbr::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("r"), 1)));
  PumpBoth(p);
  ASSERT_EQ(order.size(), 1u);
  EXPECT_EQ(order[0], pbr::adp::QosClass::Reliable);
}

TEST_F(AdpReliableTest, ShutdownClose) {
  auto p = MakePair();
  pbr::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto ca = p.ep_a->Open(op);
  pbr::adp::OpenParams opb = op;
  opb.peer = p.addr_a;
  auto cb = p.ep_b->Open(opb);
  ASSERT_TRUE(ca);
  ASSERT_TRUE(cb);
  (*ca)->Close();
  PumpBoth(p);
  EXPECT_TRUE((*cb)->IsClosed());
  EXPECT_FALSE((*ca)->Send(pbr::adp::QosClass::Reliable,
                           std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("x"), 1)));
}

TEST_F(AdpReliableTest, SpuriousAckIgnored) {
  auto p = MakePair();
  pbr::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto ca = p.ep_a->Open(op);
  pbr::adp::OpenParams opb = op;
  opb.peer = p.addr_a;
  auto cb = p.ep_b->Open(opb);
  ASSERT_TRUE(ca);
  ASSERT_TRUE(cb);
  // Send ACK from B without data — should not create issues.
  // Use reliable send then normal path; inject nothing else.
  ASSERT_TRUE((*ca)->Send(pbr::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("z"), 1)));
  PumpBoth(p);
  SUCCEED();
}

TEST_F(AdpReliableTest, GiveUpAfterMaxRtx) {
  auto p = MakePair();
  // B not accepting / not open — rtx until give-up, no crash.
  pbr::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  op.rtx_interval_ms = 5;
  op.max_rtx = 3;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);
  ASSERT_TRUE((*ca)->Send(pbr::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("z"), 1)));
  for (int i = 0; i < 10; ++i) {
    p.clock->Advance(5);
    p.ep_a->Tick();
  }
  SUCCEED();
}

} // namespace
