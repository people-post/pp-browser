#include "domain/messaging/BroadcastRpcCodec.h"
#include "domain/messaging/BroadcastJoinTicket.h"
#include "domain/messaging/BroadcastLadderLogic.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"

#include <gtest/gtest.h>
#include <variant>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

ByteVector TestBytes(uint8_t seed, size_t size = 32) {
  ByteVector bytes(size);
  for (size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<uint8_t>(seed + i);
  }
  return bytes;
}

BroadcastJoinTicketDraft SampleDraft() {
  BroadcastJoinTicketDraft draft;
  draft.publisher_peer_id = "12D3KooWPublisher";
  draft.program_id = "show-1";
  draft.join_handle = "live:show-1";
  draft.viewer_peer_id = "12D3KooWViewer";
  draft.media_epoch = 1;
  draft.hop_peer_id = "12D3KooWHop";
  draft.expires_at_ms = 2'000'000'000'000;
  return draft;
}

} // namespace

TEST(BroadcastRpcCodecTest, TicketRequestResponseRoundTrip) {
  EnsureSodiumInit();
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(keys);
  auto ticket = MintBroadcastJoinTicket(SampleDraft(), TestBytes(0x41), keys->secret_key, nullptr);
  ASSERT_TRUE(ticket) << ticket.error().message;

  BroadcastTicketRequest req;
  req.program_id = "show-1";
  req.join_handle = "live:show-1";
  req.viewer_peer_id = "12D3KooWViewer";
  auto req_json = EncodeBroadcastTicketRequest(req);
  ASSERT_TRUE(req_json);
  auto decoded_req = DecodeBroadcastRpcJson(*req_json);
  ASSERT_TRUE(decoded_req);
  ASSERT_TRUE(std::holds_alternative<BroadcastTicketRequest>(*decoded_req));
  EXPECT_EQ(std::get<BroadcastTicketRequest>(*decoded_req).viewer_peer_id, "12D3KooWViewer");

  BroadcastTicketResponse resp;
  resp.ok = true;
  resp.ticket = *ticket;
  auto resp_json = EncodeBroadcastTicketResponse(resp);
  ASSERT_TRUE(resp_json);
  auto decoded_resp = DecodeBroadcastRpcJson(*resp_json);
  ASSERT_TRUE(decoded_resp);
  ASSERT_TRUE(std::holds_alternative<BroadcastTicketResponse>(*decoded_resp));
  const auto& got = std::get<BroadcastTicketResponse>(*decoded_resp);
  ASSERT_TRUE(got.ok);
  ASSERT_TRUE(got.ticket.has_value());
  EXPECT_EQ(got.ticket->join_handle, "live:show-1");
  EXPECT_EQ(got.ticket->signature_b64, ticket->signature_b64);
}

TEST(BroadcastRpcCodecTest, ViewerAttachAdmitAndRedirectRoundTrip) {
  BroadcastLadderViewerInput in;
  in.free_viewer_slots = 0;
  in.redirect_budget = 4;
  in.self_peer_id = "parent";
  in.whitelist_online_children = {"c1", "c2"};
  const auto decision = DecideBroadcastViewerAdmit(in);
  ASSERT_EQ(decision.action, BroadcastLadderViewerAction::Redirect);

  auto result = BroadcastViewerAttachResultFromDecision(decision, "parent");
  EXPECT_EQ(result.action, BroadcastLadderViewerAction::Redirect);
  EXPECT_TRUE(result.admitted_hop_peer_id.empty());
  ASSERT_EQ(result.redirect_peer_ids.size(), 2u);

  auto json = EncodeBroadcastViewerAttachResult(result);
  ASSERT_TRUE(json);
  auto decoded = DecodeBroadcastRpcJson(*json);
  ASSERT_TRUE(decoded);
  ASSERT_TRUE(std::holds_alternative<BroadcastViewerAttachResult>(*decoded));
  const auto& got = std::get<BroadcastViewerAttachResult>(*decoded);
  EXPECT_EQ(got.action, BroadcastLadderViewerAction::Redirect);
  EXPECT_EQ(got.redirect_peer_ids, result.redirect_peer_ids);
  EXPECT_EQ(got.redirect_budget_remaining, 3);

  BroadcastViewerAttachRequest req;
  req.program_id = "show-1";
  req.join_handle = "live:show-1";
  req.viewer_peer_id = "viewer";
  req.ticket_json = "{\"v\":1}";
  req.redirect_budget = 4;
  req.path_stamp = {"L1a"};
  auto req_json = EncodeBroadcastViewerAttachRequest(req);
  ASSERT_TRUE(req_json);
  auto decoded_req = DecodeBroadcastRpcJson(*req_json);
  ASSERT_TRUE(decoded_req);
  ASSERT_TRUE(std::holds_alternative<BroadcastViewerAttachRequest>(*decoded_req));
  EXPECT_EQ(std::get<BroadcastViewerAttachRequest>(*decoded_req).path_stamp.size(), 1u);

  BroadcastLadderViewerInput admit_in;
  admit_in.free_viewer_slots = 1;
  auto admit = BroadcastViewerAttachResultFromDecision(DecideBroadcastViewerAdmit(admit_in), "hop-self");
  EXPECT_EQ(admit.action, BroadcastLadderViewerAction::Admit);
  EXPECT_EQ(admit.admitted_hop_peer_id, "hop-self");
}

TEST(BroadcastRpcCodecTest, RelaySlotWinDemoteRoundTrip) {
  BroadcastLadderSlotWinInput in;
  in.free_child_slots = 0;
  in.candidate_on_whitelist = true;
  in.new_relay_peer_id = "relay-1";
  in.demotable_viewer_peer_ids = {"viewer-a"};
  const auto decision = DecideBroadcastSlotWin(in);
  ASSERT_EQ(decision.action, BroadcastLadderSlotWinAction::DemoteViewersAndAdmitRelay);

  auto result = BroadcastRelaySlotWinResultFromDecision(decision);
  auto json = EncodeBroadcastRelaySlotWinResult(result);
  ASSERT_TRUE(json);
  auto decoded = DecodeBroadcastRpcJson(*json);
  ASSERT_TRUE(decoded);
  ASSERT_TRUE(std::holds_alternative<BroadcastRelaySlotWinResult>(*decoded));
  const auto& got = std::get<BroadcastRelaySlotWinResult>(*decoded);
  EXPECT_EQ(got.action, BroadcastLadderSlotWinAction::DemoteViewersAndAdmitRelay);
  ASSERT_EQ(got.demote_viewer_peer_ids.size(), 1u);
  EXPECT_EQ(got.demote_viewer_peer_ids[0], "viewer-a");
  EXPECT_EQ(got.demotion_redirect_target, "relay-1");

  BroadcastRelaySlotWinRequest req;
  req.program_id = "show-1";
  req.join_handle = "live:show-1";
  req.relay_peer_id = "relay-1";
  auto req_json = EncodeBroadcastRelaySlotWinRequest(req);
  ASSERT_TRUE(req_json);
  ASSERT_TRUE(DecodeBroadcastRpcJson(*req_json));
}

TEST(BroadcastRpcCodecTest, RejectsUnknownOp) {
  EXPECT_FALSE(DecodeBroadcastRpcJson(R"({"op":"nope"})"));
}

} // namespace pbr
