#include "domain/messaging/CallTxOnlyEscalateLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

CallTxOnlyEscalateDecisionInput BaseReadyInput() {
  CallTxOnlyEscalateDecisionInput in;
  in.has_circuit_reach = true;
  in.direct_active = true;
  in.active_call_id = "call:1";
  in.media_call_id = "call:1";
  in.tx_audio_frames = kCallTxOnlyEscalateMinTxFrames;
  in.direct_connected_at_ms = 1000;
  in.now_ms = 1000 + kCallTxOnlyEscalateGraceMs;
  in.peer_nonempty = true;
  return in;
}

TEST(CallTxOnlyEscalateLogicTest, EscalatesWhenTxOnlyPastGrace) {
  EXPECT_TRUE(ShouldEscalateTxOnlyDirect(BaseReadyInput()));
}

TEST(CallTxOnlyEscalateLogicTest, SkipsWhenAlreadyOnCircuit) {
  auto in = BaseReadyInput();
  in.media_path_kind = "circuit";
  EXPECT_FALSE(ShouldEscalateTxOnlyDirect(in));
}

TEST(CallTxOnlyEscalateLogicTest, SkipsWhenRxPresent) {
  auto in = BaseReadyInput();
  in.rx_audio_frames = 1;
  EXPECT_FALSE(ShouldEscalateTxOnlyDirect(in));
}

TEST(CallTxOnlyEscalateLogicTest, SkipsBeforeGrace) {
  auto in = BaseReadyInput();
  in.now_ms = in.direct_connected_at_ms + kCallTxOnlyEscalateGraceMs - 1;
  EXPECT_FALSE(ShouldEscalateTxOnlyDirect(in));
}

TEST(CallTxOnlyEscalateLogicTest, SkipsWhenSfuAttached) {
  auto in = BaseReadyInput();
  in.sfu_attached = true;
  EXPECT_FALSE(ShouldEscalateTxOnlyDirect(in));
}

TEST(CallTxOnlyEscalateLogicTest, SkipsWhenAlreadyDone) {
  auto in = BaseReadyInput();
  in.already_done = true;
  EXPECT_FALSE(ShouldEscalateTxOnlyDirect(in));
}

TEST(CallTxOnlyEscalateLogicTest, NeverSoftMigratePathKind) {
  // V038: escalate is circuit Ensure, not media_relay SoftMigrate.
  auto in = BaseReadyInput();
  in.media_path_kind = "direct";
  EXPECT_TRUE(ShouldEscalateTxOnlyDirect(in));
}

} // namespace
} // namespace pbr
