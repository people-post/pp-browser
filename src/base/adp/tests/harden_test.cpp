#include "base/adp/Clock.h"
#include "base/adp/Endpoint.h"
#include "base/adp/HmacBinder.h"
#include "base/adp/MemoryDatagramIo.h"
#include "base/adp/OsUdpDatagramIo.h"
#include "base/adp/Types.h"
#include "base/adp/WireCodec.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <random>
#include <string>
#include <vector>

namespace {

pbr::adp::PeerKey Key() {
  pbr::adp::PeerKey k;
  k.bytes.fill(0x77);
  return k;
}

pbr::adp::AssocId Aid() {
  pbr::adp::AssocId id;
  id.bytes.fill(0x88);
  return id;
}

class AdpHardenTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

TEST_F(AdpHardenTest, PacketMutilatorNeverCrashes) {
  pbr::adp::WirePacket pkt;
  pkt.type = pbr::adp::PacketType::DataBestEffort;
  pkt.assoc = Aid();
  pkt.seq = 1;
  pkt.timestamp_ms = 42;
  pkt.payload = {9, 8, 7};
  auto enc = pbr::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  auto sealed = pbr::adp::HmacBinder(Key()).Seal(*enc);
  ASSERT_TRUE(sealed);

  std::mt19937 rng(12345);
  for (int i = 0; i < 200; ++i) {
    auto mut = *sealed;
    const size_t nflip = 1 + (rng() % 8);
    for (size_t f = 0; f < nflip; ++f) {
      mut[rng() % mut.size()] ^= static_cast<uint8_t>(1 + (rng() % 255));
    }
    if (rng() % 2 == 0 && mut.size() > 4) {
      mut.resize(rng() % mut.size());
    }
    (void)pbr::adp::WireCodec::Decode(mut);
    (void)pbr::adp::HmacBinder(Key()).Verify(mut);
  }
}

TEST_F(AdpHardenTest, OsUdpLoopbackSmoke) {
  auto bound_a = pbr::adp::OsUdpDatagramIo::Bind(pbr::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  auto bound_b = pbr::adp::OsUdpDatagramIo::Bind(pbr::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  ASSERT_TRUE(bound_a);
  ASSERT_TRUE(bound_b);
  std::shared_ptr<pbr::adp::DatagramIo> io_a(std::move(*bound_a));
  std::shared_ptr<pbr::adp::DatagramIo> io_b(std::move(*bound_b));
  auto clock = std::make_shared<pbr::adp::VirtualClock>(9'000'000);
  auto ep_a = std::make_unique<pbr::adp::Endpoint>(io_a, clock);
  auto ep_b = std::make_unique<pbr::adp::Endpoint>(io_b, clock);
  const auto addr_a = ep_a->Io().LocalEndpoint();
  const auto addr_b = ep_b->Io().LocalEndpoint();

  pbr::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = addr_b;
  auto ca = ep_a->Open(op);
  ASSERT_TRUE(ca);

  std::string got;
  pbr::adp::OpenParams opb = op;
  opb.peer = addr_a;
  auto cb = ep_b->Open(opb);
  ASSERT_TRUE(cb);
  (*cb)->OnMessage([&](const pbr::adp::Message& m) {
    got.assign(m.payload.begin(), m.payload.end());
  });

  ASSERT_TRUE((*ca)->Send(pbr::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("udp"), 3)));
  for (int i = 0; i < 50; ++i) {
    ep_a->Pump();
    ep_b->Pump();
    ep_a->Tick();
    ep_b->Tick();
    if (!got.empty()) {
      break;
    }
    clock->Advance(5);
  }
  EXPECT_EQ(got, "udp");
}

TEST_F(AdpHardenTest, MultiConnectionStressMemory) {
  auto clock = std::make_shared<pbr::adp::VirtualClock>(1);
  auto hub = pbr::adp::MemoryDatagramIo::MakeHub();
  auto addr_a = pbr::adp::IpEndpoint::V4(10, 1, 1, 1, 1);
  auto addr_b = pbr::adp::IpEndpoint::V4(10, 1, 1, 2, 2);
  auto io_a = std::make_shared<pbr::adp::MemoryDatagramIo>(hub, addr_a);
  auto io_b = std::make_shared<pbr::adp::MemoryDatagramIo>(hub, addr_b);
  auto ep_a = std::make_unique<pbr::adp::Endpoint>(io_a, clock);
  auto ep_b = std::make_unique<pbr::adp::Endpoint>(io_b, clock);
  ep_b->SetAcceptKey(Key());
  ep_b->SetAcceptEnabled(true);

  constexpr int N = 32;
  std::vector<std::shared_ptr<pbr::adp::Connection>> cons;
  size_t received = 0;
  for (int i = 0; i < N; ++i) {
    pbr::adp::OpenParams op;
    op.key = Key();
    op.id = Aid();
    op.id.bytes[0] = static_cast<uint8_t>(i);
    op.mint_id = false;
    op.peer = addr_b;
    auto c = ep_a->Open(op);
    ASSERT_TRUE(c);
    cons.push_back(*c);
    pbr::adp::OpenParams opb = op;
    opb.peer = addr_a;
    auto cb = ep_b->Open(opb);
    ASSERT_TRUE(cb);
    (*cb)->OnMessage([&](const pbr::adp::Message&) { ++received; });
  }
  for (int i = 0; i < N; ++i) {
    const uint8_t b = static_cast<uint8_t>(i);
    ASSERT_TRUE(cons[static_cast<size_t>(i)]->Send(pbr::adp::QosClass::BestEffort,
                                                   std::span<const uint8_t>(&b, 1)));
  }
  ep_b->Pump();
  EXPECT_EQ(received, static_cast<size_t>(N));
}

} // namespace
