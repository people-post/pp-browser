#include "domain/call_media/CallMediaAmpTransport.h"
#include "domain/call_media/CallMediaLegCoordinator.h"
#include "mesh/testing/mesh_test_harness.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>

namespace pbr {
namespace {

/**
 * One transport serves a device's calls in turn (CallMediaPlane builds one per mesh; each call's
 * 1:1 path borrows it, V056). Its hang-up and detach name the call: a late stop of an ended call
 * must not say `bye` on, or tear down, the next call's leg (K014).
 */
class CallMediaAmpTransportTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_GE(sodium_init(), 0);
    auto created = mesh::test::AmpMeshHarness::Create();
    ASSERT_TRUE(static_cast<bool>(created));
    harness_ = std::move(*created);
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_a().RegisterEndpoint("b", harness_->ma_b)));
    ASSERT_TRUE(static_cast<bool>(harness_->mgr_b().RegisterEndpoint("a", harness_->ma_a)));

    a_ = std::make_unique<CallMediaAmpTransport>(*harness_->runtime_a, *harness_->clock,
                                                 CallMediaAmpTransport::IoPump{});
    b_ = std::make_unique<CallMediaLegCoordinator>(*harness_->runtime_b, *harness_->clock);
    a_->Start();
    b_->Start();
    b_->SetInboundHandler(AnswerInline([this](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
      params.media_key = key_;
      params.media_epoch = 1;
      params.offerer = false;
      cbs.on_connected = [this] { b_connected_.fetch_add(1, std::memory_order_acq_rel); };
      cbs.on_remote_ended = [this] { b_remote_ended_.fetch_add(1, std::memory_order_acq_rel); };
    }));
  }

  void TearDown() override {
    a_->Stop();
    b_->Stop();
    a_.reset();
    b_.reset();
    harness_.reset();
  }

  /** Connect `call_id` from A and wait until both ends carry its media. */
  void Connect(const std::string& call_id, const int b_connects) {
    CallMediaDirectConnectParams params;
    params.peer_key = "b";
    params.call_id = call_id;
    params.media_epoch = 1;
    params.media_key = key_;
    params.offerer = true;
    std::optional<bool> ok;
    a_->ConnectAsync(params, {}, [&ok](CallMediaRoe r) { ok = static_cast<bool>(r); }, 5000);
    harness_->PumpUntil([&] {
      return ok.has_value() && b_connected_.load(std::memory_order_acquire) >= b_connects;
    });
    ASSERT_TRUE(ok.value_or(false)) << call_id;
    ASSERT_EQ(a_->Phase(), CallMediaSessionPhase::MediaReady) << call_id;
    ASSERT_EQ(b_->Phase(), CallMediaSessionPhase::MediaReady) << call_id;
  }

  const ByteVector key_ = ByteVector(32, 0x5a);
  std::unique_ptr<mesh::test::AmpMeshHarness> harness_;
  std::unique_ptr<CallMediaAmpTransport> a_;
  std::unique_ptr<CallMediaLegCoordinator> b_;
  std::atomic<int> b_connected_{0};
  std::atomic<int> b_remote_ended_{0};
};

TEST_F(CallMediaAmpTransportTest, LateHangUpOfAnEndedCallLeavesTheNextCallsLegAlone) {
  Connect("call-first", 1);
  a_->HangUp("call-first");
  harness_->PumpUntil([&] { return b_remote_ended_.load(std::memory_order_acquire) >= 1; });
  ASSERT_EQ(b_remote_ended_.load(), 1);
  ASSERT_EQ(b_->Phase(), CallMediaSessionPhase::Idle);

  Connect("call-second", 2);

  // The first call's path stops again, late (its own stop, after the device moved on).
  a_->HangUp("call-first");
  a_->Detach("call-first");
  for (int i = 0; i < 20; ++i) {
    harness_->PumpBoth();
  }
  EXPECT_EQ(b_remote_ended_.load(), 1) << "no bye reached the second call";
  EXPECT_EQ(a_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(a_->ActiveParams().call_id, "call-second");
  EXPECT_TRUE(a_->SendAudio({0x01, 0x02}, 1));

  // The second call's own hang-up still ends it for the peer.
  a_->HangUp("call-second");
  harness_->PumpUntil([&] { return b_remote_ended_.load(std::memory_order_acquire) >= 2; });
  EXPECT_EQ(b_remote_ended_.load(), 2);
  EXPECT_EQ(a_->Phase(), CallMediaSessionPhase::Idle);
}

// A call that never had media on this transport has nothing to hang up: the live call is kept.
TEST_F(CallMediaAmpTransportTest, HangUpOrDetachOfAnUnknownCallDropsNothing) {
  Connect("call-live", 1);
  a_->HangUp("call-other");
  a_->Detach("call-other");
  a_->HangUp({});
  a_->Detach({});
  for (int i = 0; i < 20; ++i) {
    harness_->PumpBoth();
  }
  EXPECT_EQ(b_remote_ended_.load(), 0);
  EXPECT_EQ(a_->Phase(), CallMediaSessionPhase::MediaReady);
  EXPECT_EQ(b_->Phase(), CallMediaSessionPhase::MediaReady);
}

} // namespace
} // namespace pbr
