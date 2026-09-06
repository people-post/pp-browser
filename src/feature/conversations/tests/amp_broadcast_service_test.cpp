#include "feature/conversations/AmpBroadcastTransport.h"

#include "domain/messaging/BroadcastJoinTicket.h"
#include "domain/mesh/tests/support/mesh_test_harness.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

ByteVector TestKeyBytes(uint8_t seed) {
  ByteVector bytes(32);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i));
  }
  return bytes;
}

class AmpBroadcastTransportTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = pbr::test::AmpMeshHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created));
    harness_ = std::move(*created);

    ASSERT_TRUE(static_cast<bool>(harness_->chat_a().RegisterEndpoint("b", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->chat_b().RegisterEndpoint("a", harness_->ma_a)));
    harness_->ep_a->SetAcceptEnabled(true);

    auto keys_a = MlDsa::GenerateKeyPair();
    auto keys_b = MlDsa::GenerateKeyPair();
    ASSERT_TRUE(keys_a);
    ASSERT_TRUE(keys_b);
    pk_a_ = keys_a->public_key;
    sk_a_ = keys_a->secret_key;
    pk_b_ = keys_b->public_key;

    AmpBroadcastTransport::WorkerPost no_worker;
    a_svc_ = std::make_unique<AmpBroadcastTransport>(harness_->chat_a(), [this] { harness_->PumpBoth(); }, no_worker);
    b_svc_ = std::make_unique<AmpBroadcastTransport>(harness_->chat_b(), [this] { harness_->PumpBoth(); }, no_worker);

    auto resolve = [this](const std::string& peer_id) -> std::optional<ByteVector> {
      if (peer_id == "publisher-a") {
        return pk_a_;
      }
      if (peer_id == "publisher-b") {
        return pk_b_;
      }
      return std::nullopt;
    };
    a_svc_->SetPublisherKeyResolver(resolve);
    b_svc_->SetPublisherKeyResolver(resolve);
    a_svc_->SetPublisherSecretResolver([this]() -> std::optional<ByteVector> { return sk_a_; });
    a_svc_->SetNowMsResolver([]() { return int64_t{1'900'000'000'000}; });
    b_svc_->SetNowMsResolver([]() { return int64_t{1'900'000'000'000}; });

    AmpBroadcastTransport::LiveProgramKey live;
    live.publisher_peer_id = "publisher-a";
    live.media_key_bytes = TestKeyBytes(0x40);
    live.media_epoch = 1;
    live.media_key_id = "mk:test";
    live.hop_peer_id = "hop-1";
    live.expires_at_ms = 2'000'000'000'000;
    a_svc_->PutLiveProgramKey("show-1", "live:show-1", live);

    a_svc_->SetHopAttachResolver([](const std::string&, const std::string&) {
      AmpBroadcastTransport::HopAttachContext hop;
      hop.free_viewer_slots = 1;
      hop.self_peer_id = "hop-a";
      return hop;
    });
    a_svc_->SetHopSlotWinResolver([](const std::string&, const std::string&, const std::string&) {
      AmpBroadcastTransport::HopSlotWinContext hop;
      hop.free_child_slots = 0;
      hop.candidate_on_whitelist = true;
      hop.demotable_viewer_peer_ids = {"viewer-piped"};
      hop.max_demotions = 1;
      return hop;
    });

    a_svc_->Start();
    b_svc_->Start();
  }

  void TearDown() override {
    if (a_svc_) {
      a_svc_->Stop();
    }
    if (b_svc_) {
      b_svc_->Stop();
    }
    a_svc_.reset();
    b_svc_.reset();
    harness_.reset();
  }

  std::unique_ptr<pbr::test::AmpMeshHarness> harness_;
  std::unique_ptr<AmpBroadcastTransport> a_svc_;
  std::unique_ptr<AmpBroadcastTransport> b_svc_;
  ByteVector pk_a_;
  ByteVector pk_b_;
  ByteVector sk_a_;
};

TEST_F(AmpBroadcastTransportTest, RequestTicketRoundTripMintsSignedTicket) {
  BroadcastTicketRequest req;
  req.program_id = "show-1";
  req.join_handle = "live:show-1";
  req.viewer_peer_id = "viewer-1";

  auto resp = b_svc_->RequestTicket("a", req);
  ASSERT_TRUE(resp) << resp.error().message;
  EXPECT_TRUE(resp->ok) << resp->error;
  ASSERT_TRUE(resp->ticket.has_value());
  EXPECT_EQ(resp->ticket->viewer_peer_id, "viewer-1");
  EXPECT_EQ(resp->ticket->program_id, "show-1");
  EXPECT_FALSE(resp->ticket->signature_b64.empty());

  auto verified = VerifyBroadcastJoinTicket(*resp->ticket, pk_a_, 1'900'000'000'000, "viewer-1");
  ASSERT_TRUE(verified) << verified.error().message;
}

TEST_F(AmpBroadcastTransportTest, RequestViewerAttachAdmitsWithValidTicket) {
  BroadcastTicketRequest treq;
  treq.program_id = "show-1";
  treq.join_handle = "live:show-1";
  treq.viewer_peer_id = "viewer-1";
  auto tresp = b_svc_->RequestTicket("a", treq);
  ASSERT_TRUE(tresp) << tresp.error().message;
  ASSERT_TRUE(tresp->ok) << tresp->error;
  ASSERT_TRUE(tresp->ticket.has_value());

  auto ticket_json = EncodeBroadcastJoinTicketJson(*tresp->ticket);
  ASSERT_TRUE(ticket_json) << ticket_json.error().message;

  BroadcastViewerAttachRequest areq;
  areq.program_id = "show-1";
  areq.join_handle = "live:show-1";
  areq.viewer_peer_id = "viewer-1";
  areq.ticket_json = *ticket_json;
  auto aresp = b_svc_->RequestViewerAttach("a", areq);
  ASSERT_TRUE(aresp) << aresp.error().message;
  EXPECT_EQ(aresp->action, BroadcastLadderViewerAction::Admit);
  EXPECT_EQ(aresp->admitted_hop_peer_id, "hop-a");
}

TEST_F(AmpBroadcastTransportTest, RequestRelaySlotWinDemotesViewerWhenFull) {
  BroadcastRelaySlotWinRequest req;
  req.program_id = "show-1";
  req.join_handle = "live:show-1";
  req.relay_peer_id = "relay-new";
  auto resp = b_svc_->RequestRelaySlotWin("a", req);
  ASSERT_TRUE(resp) << resp.error().message;
  EXPECT_EQ(resp->action, BroadcastLadderSlotWinAction::DemoteViewersAndAdmitRelay);
  ASSERT_FALSE(resp->demote_viewer_peer_ids.empty());
  EXPECT_EQ(resp->demote_viewer_peer_ids.front(), "viewer-piped");
  EXPECT_EQ(resp->demotion_redirect_target, "relay-new");
}

} // namespace
} // namespace pbr
