#include "base/mesh/channel/AmpChannelLimits.h"
#include "base/mesh/channel/ChannelPolicy.h"
#include "base/mesh/channel/ChannelWire.h"
#include "base/mesh/channel/Types.h"
#include "base/mesh/link/Types.h"
#include "base/mesh/link/tests/amp_integration_harness.h"
#include "base/mesh/session/SessionControl.h"

#include <gtest/gtest.h>
#include <sodium.h>

namespace pbr::test {
namespace {

amp::ChannelPolicy BulkPolicy() {
  amp::ChannelPolicy policy;
  policy.cls = amp::ChannelClass::Bulk;
  policy.drop = amp::ChannelDropPolicy::Never;
  policy.max_outbound_frames = amp::AmpChannelLimits::kMaxControlOutboundFrames;
  policy.max_message_bytes = amp::AmpChannelLimits::kMaxChatBlobFrameBytes;
  return policy;
}

amp::ChannelPolicy RealtimeBulkPolicy() {
  amp::ChannelPolicy policy;
  policy.cls = amp::ChannelClass::Realtime;
  policy.drop = amp::ChannelDropPolicy::Never;
  policy.max_outbound_frames = amp::AmpChannelLimits::kMaxControlOutboundFrames;
  policy.max_message_bytes = amp::AmpChannelLimits::kMaxChatBlobFrameBytes;
  return policy;
}

class AmpIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

TEST_F(AmpIntegrationTest, ChannelResetSiblingSurvives) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch_chat = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", amp::ControlJsonChannelPolicy());
  const auto ch_hist =
      h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat-history/1.0.0", amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch_chat.has_value());
  ASSERT_TRUE(ch_hist.has_value());

  h.PumpUntil([&] {
    auto* link = h.mgr_a().FindLink("b");
    return link && link->Mux() && link->Mux()->State(*ch_chat) == amp::ChannelState::Open
           && link->Mux()->State(*ch_hist) == amp::ChannelState::Open;
  });

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch_hist, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  auto* outbound = h.mgr_a().FindLink("b");
  ASSERT_NE(outbound, nullptr);
  ASSERT_TRUE(static_cast<bool>(outbound->Mux()->ResetChannel(*ch_chat)));
  h.PumpBoth();
  EXPECT_EQ(outbound->Mux()->State(*ch_chat), amp::ChannelState::Closed);
  EXPECT_EQ(outbound->Mux()->State(*ch_hist), amp::ChannelState::Open);

  const std::vector<uint8_t> msg = {'o', 'k'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch_hist, msg));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg; }));
  EXPECT_EQ(received, msg);
}

TEST_F(AmpIntegrationTest, ReliableDataSurvivesLoss) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  h.ConfigureLoss(HarnessSide::A, 3);
  const std::vector<uint8_t> msg = {'l', 'o', 's', 's'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  for (int i = 0; i < 40; ++i) {
    h.AdvanceMs(15);
  }
  EXPECT_EQ(received, msg);
}

TEST_F(AmpIntegrationTest, DualDialChannelsWorkAfterElection) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.DualAssociate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  const std::vector<uint8_t> msg = {'d', 'u', 'a', 'l'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg; }));
  EXPECT_EQ(received, msg);
}

TEST_F(AmpIntegrationTest, MshFailureNoConnectedMux) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  h.ep_b->SetAcceptEnabled(true);
  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b", h.ma_b)));

  h.io_a->SetDropRate(1.0);
  h.io_b->SetDropRate(1.0);
  bool done = false;
  bool ok = true;
  h.mgr_a().EnsureAssociation("b", [&](Roe<void> result) {
    ok = result.isOk();
    done = true;
  });
  for (size_t i = 0; i < 300; ++i) {
    h.PumpBoth();
  }
  EXPECT_FALSE(h.mgr_a().IsConnected("b"));
  EXPECT_EQ(h.mgr_b().FindConnectedInboundLink(), nullptr);
  if (done) {
    EXPECT_FALSE(ok);
  }
}

TEST_F(AmpIntegrationTest, AssociationCloseAndRecovery) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  auto* outbound = h.mgr_a().FindLink("b");
  ASSERT_NE(outbound, nullptr);
  auto* conn = outbound->ConnectionOrNull();
  ASSERT_NE(conn, nullptr);
  conn->Close();

  for (int i = 0; i < 50; ++i) {
    h.AdvanceMs(100);
  }
  EXPECT_FALSE(h.mgr_a().IsConnected("b"));

  ASSERT_TRUE(h.Associate());
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}

TEST_F(AmpIntegrationTest, PathMigrateMidSession) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  auto* inbound_conn = inbound->ConnectionOrNull();
  ASSERT_NE(inbound_conn, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  const std::vector<uint8_t> msg1 = {'a'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg1));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg1; }));

  const adp::IpEndpoint alt_a = adp::IpEndpoint::V4(10, 0, 0, 1, 1001);
  const std::vector<uint8_t> ping = {'p'};
  ASSERT_TRUE(h.SendSealedFromAlternatePath(HarnessSide::A, "b", *ch, alt_a, ping, 2));
  for (int i = 0; i < 20; ++i) {
    h.PumpBoth();
  }
  EXPECT_EQ(inbound_conn->PeerEndpoint(), alt_a);

  const std::vector<uint8_t> msg2 = {'b'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg2));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg2; }));
  EXPECT_EQ(inbound_conn->PeerEndpoint(), h.addr_a);
  EXPECT_EQ(inbound->Mux()->State(*ch), amp::ChannelState::Open);
}

TEST_F(AmpIntegrationTest, LargeFragRoundTripThroughStack) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat-blob/1.0.0", BulkPolicy());
  ASSERT_TRUE(ch.has_value());

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  std::vector<uint8_t> large(2500, 0xAB);
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, large));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == large; }));
  EXPECT_EQ(received, large);
}

TEST_F(AmpIntegrationTest, WireRekeyWithGraceWindow) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());
  h.PumpBoth();

  auto* link_a = h.mgr_a().FindLink("b");
  auto* link_b = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(link_a, nullptr);
  ASSERT_NE(link_b, nullptr);
  ASSERT_NE(link_a->GetSession(), nullptr);
  const uint32_t epoch_before = link_a->GetSession()->Material().session_epoch;

  amp::ChannelFrame frame;
  frame.header.frame_type = amp::ChannelFrameType::Data;
  frame.header.channel_id = *ch;
  frame.header.channel_seq = 99;
  frame.payload = {'g', 'r', 'a', 'c', 'e'};
  auto wire = amp::ChannelWire::Encode(frame);
  ASSERT_TRUE(static_cast<bool>(wire));
  auto stale_sealed = link_a->GetSession()->Seal(*ch, 99, *wire);
  ASSERT_TRUE(static_cast<bool>(stale_sealed));

  ASSERT_TRUE(h.RequestRekey(HarnessSide::A, "b"));
  EXPECT_EQ(link_a->GetSession()->Material().session_epoch, epoch_before + 1);
  EXPECT_EQ(link_b->GetSession()->Material().session_epoch, epoch_before + 1);

  auto opened = link_b->GetSession()->Open(*ch, 99, *stale_sealed, h.clock->NowMs());
  ASSERT_TRUE(static_cast<bool>(opened));
  auto decoded = amp::ChannelWire::Decode(*opened);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->payload, frame.payload);

  const std::vector<uint8_t> msg = {'n', 'e', 'w'};
  std::vector<uint8_t> received;
  link_b->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg; }));
}

TEST_F(AmpIntegrationTest, PostGraceStaleEpochDropped) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());

  auto* link_a = h.mgr_a().FindLink("b");
  auto* link_b = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(link_a, nullptr);
  ASSERT_NE(link_b, nullptr);

  amp::ChannelFrame frame;
  frame.header.frame_type = amp::ChannelFrameType::Data;
  frame.header.channel_id = *ch;
  frame.header.channel_seq = 77;
  frame.payload = {'s', 't', 'a', 'l', 'e'};
  auto wire = amp::ChannelWire::Encode(frame);
  ASSERT_TRUE(static_cast<bool>(wire));
  auto stale_sealed = link_a->GetSession()->Seal(*ch, 77, *wire);
  ASSERT_TRUE(static_cast<bool>(stale_sealed));

  ASSERT_TRUE(h.RequestRekey(HarnessSide::A, "b"));
  h.AdvanceMs(amp::kSessionRekeyGraceMs + 1);

  auto opened = link_b->GetSession()->Open(*ch, 77, *stale_sealed, h.clock->NowMs());
  EXPECT_FALSE(static_cast<bool>(opened));

  const std::vector<uint8_t> msg = {'f', 'r', 'e', 's', 'h'};
  std::vector<uint8_t> received;
  link_b->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg; }));
}

TEST_F(AmpIntegrationTest, AdversarialDialTimeoutAdv04) {
  amp::PeerLinkConfig cfg = AmpMeshTestLinkConfig();
  cfg.dial_timeout = std::chrono::milliseconds(200);
  auto created = MakeAmpIntegrationHarness(cfg);
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  h.ep_b->SetAcceptEnabled(false);
  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b", h.ma_b)));

  bool done = false;
  bool ok = true;
  h.mgr_a().EnsureAssociation("b", [&](Roe<void> result) {
    ok = result.isOk();
    done = true;
  });
  h.AdvanceMs(250);
  EXPECT_TRUE(done);
  EXPECT_FALSE(ok);
  EXPECT_FALSE(h.mgr_a().IsConnected("b"));
}

TEST_F(AmpIntegrationTest, AdversarialMaxLinksAdv02) {
  amp::PeerLinkConfig cfg = AmpMeshTestLinkConfig();
  cfg.max_links = 1;
  auto created = MakeAmpIntegrationHarness(cfg);
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;

  h.ep_b->SetAcceptEnabled(false);
  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b", h.ma_b)));
  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b2", h.ma_b)));
  h.mgr_a().EnsureAssociation("b", {});
  EXPECT_EQ(h.CountLinks(HarnessSide::A), 1);

  bool dial_done = false;
  bool dial_ok = true;
  h.mgr_a().EnsureAssociation("b2", [&](Roe<void> result) {
    dial_ok = result.isOk();
    dial_done = true;
  });
  EXPECT_TRUE(dial_done);
  EXPECT_FALSE(dial_ok);
  EXPECT_EQ(h.CountLinks(HarnessSide::A), 1);

  h.ep_b->SetAcceptEnabled(true);
  ASSERT_TRUE(h.Associate());
  EXPECT_EQ(h.CountLinks(HarnessSide::B), 1);

  adp::OpenParams params;
  params.key = amp::PreSessionPeerKey();
  params.mint_id = true;
  params.peer = h.addr_b;
  h.AdvanceMs(1);
  auto extra = h.ep_a->Open(params);
  ASSERT_TRUE(static_cast<bool>(extra));
  (void)(*extra)->Send(adp::QosClass::Reliable, std::vector<uint8_t>{0xDE, 0xAD});
  h.PumpBoth();
  EXPECT_EQ(h.CountLinks(HarnessSide::B), 1);
}

TEST_F(AmpIntegrationTest, AdversarialGarbageMshMidHandshakeAdv03) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  h.ep_b->SetAcceptEnabled(true);
  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b", h.ma_b)));

  bool done = false;
  bool ok = false;
  h.mgr_a().EnsureAssociation("b", [&](Roe<void> result) {
    ok = result.isOk();
    done = true;
  });
  for (size_t i = 0; i < 500; ++i) {
    if (h.mgr_a().FindLink("b") != nullptr) {
      h.InjectMshGarbage(HarnessSide::A, "b");
    }
    h.InjectRawDatagram(HarnessSide::A, h.addr_b, std::vector<uint8_t>(32, 0xCC));
    h.PumpBoth();
    if (done && h.mgr_a().IsConnected("b")) {
      break;
    }
  }
  ASSERT_TRUE(done);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}

TEST_F(AmpIntegrationTest, AdversarialSealedGarbageFloodAdv06) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());

  for (uint32_t i = 0; i < 500; ++i) {
    h.InjectSealedGarbage(HarnessSide::A, "b", *ch, i + 1000);
  }
  h.PumpBudget(30);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
  EXPECT_TRUE(h.mgr_b().FindLinkByPeerId(h.peer_id_a) != nullptr);

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  const std::vector<uint8_t> msg = {'f', 'l', 'o', 'o', 'd'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg; }));
}

TEST_F(AmpIntegrationTest, AdversarialFragPartialBombAdv08) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat-blob/1.0.0", RealtimeBulkPolicy());
  ASSERT_TRUE(ch.has_value());

  for (uint64_t msg_id = 1; msg_id <= 25; ++msg_id) {
    ASSERT_TRUE(h.InjectPartialFrag(HarnessSide::A, "b", *ch, 1, msg_id, 0, 50, 50'000,
                                    std::vector<uint8_t>(500, 0xBB)));
  }
  h.PumpBudget(10);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  const std::vector<uint8_t> msg = {'b', 'o', 'm', 'b'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg; }));

  h.mgr_a().MarkWarm("b");
  h.AdvanceMs(amp::kDefaultFragAssemblyTimeoutMs + 100);
  h.PumpBudget(5);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}

} // namespace
} // namespace pbr::test
