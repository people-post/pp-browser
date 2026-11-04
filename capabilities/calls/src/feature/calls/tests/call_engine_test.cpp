#include "feature/calls/CallEngine.h"
#include "feature/calls/CallMedia.h"
#include "feature/calls/tests/call_direct_driver_fake.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>

namespace pbr {
namespace {

// V057 1: a call's paths reach the engine only through the handle its media lends them.
class CallEngineTest : public ::testing::Test {
protected:
  void SetUp() override {
    engine_.SetSkipDeviceOpenForTest(true);
    resources_.engine = &engine_;
    // Keep the handle each call's 1:1 path is built with: what the path sees.
    resources_.make_direct = [this](const std::string& call_id, uint64_t, std::shared_ptr<CallEngine> engine,
                                    std::shared_ptr<std::atomic<bool>>) -> std::unique_ptr<CallDirectDriver> {
      lent_[call_id] = std::move(engine);
      return std::make_unique<test::NullDirectDriver>();
    };
  }
  void TearDown() override { engine_.Stop(); }

  /** The handle `media`'s 1:1 path was built with. */
  std::shared_ptr<CallEngine> LentTo(CallMedia& media) {
    (void)media.Direct();
    return lent_[media.CallId()];
  }

  static MediaEngine::PacketSendFn NoSend() {
    return [](const MediaEngine::MediaPacket&) {};
  }

  MediaEngine engine_;
  CallMediaResources resources_;
  std::map<std::string, std::shared_ptr<CallEngine>> lent_;
};

TEST_F(CallEngineTest, APathKnowsWhetherTheEngineRunsItsCallAndOnWhichPath) {
  CallMedia first("call:1", resources_);
  CallMedia second("call:2", resources_);
  auto mine = LentTo(first);
  auto other = LentTo(second);
  ASSERT_TRUE(mine && other);
  EXPECT_FALSE(mine->Runs());
  EXPECT_FALSE(mine->RunsAnother());

  ASSERT_TRUE(first.StartEngine(CallMediaPath::Hop, NoSend()));
  EXPECT_TRUE(mine->Runs());
  EXPECT_TRUE(mine->RunsOn(CallMediaPath::Hop));
  EXPECT_FALSE(mine->RunsOn(CallMediaPath::Direct)) << "started for the hop, not the 1:1";
  EXPECT_FALSE(other->Runs());
  EXPECT_TRUE(other->RunsAnother()) << "another call's path sees that someone else holds the media";

  ASSERT_TRUE(first.StartEngine(CallMediaPath::Direct, NoSend()));
  EXPECT_TRUE(mine->RunsOn(CallMediaPath::Direct)) << "a restart on the 1:1 moves the fact with it";
  EXPECT_FALSE(mine->RunsOn(CallMediaPath::Hop));
}

TEST_F(CallEngineTest, AStopFromAnywhereEndsIt) {
  CallMedia first("call:1", resources_);
  auto mine = LentTo(first);
  ASSERT_TRUE(first.StartEngine(CallMediaPath::Direct, NoSend()));
  ASSERT_TRUE(mine->RunsOn(CallMediaPath::Direct));
  engine_.Stop(); // the device stopping a leftover, the seat letting go: not the call's media
  EXPECT_FALSE(mine->Runs());
  EXPECT_FALSE(mine->RunsOn(CallMediaPath::Direct));
}

TEST_F(CallEngineTest, WritesReachTheEngineOnlyWhileItRunsTheCall) {
  CallMedia first("call:1", resources_);
  CallMedia second("call:2", resources_);
  auto mine = LentTo(first);
  auto other = LentTo(second);
  ASSERT_TRUE(first.StartEngine(CallMediaPath::Direct, NoSend()));
  MediaEngine::MediaPacket packet;
  packet.stream_id = 7;
  packet.payload = {1, 2, 3};
  EXPECT_TRUE(mine->Deliver(packet));
  EXPECT_FALSE(other->Deliver(packet)) << "a packet for another call never reaches this call's session";
}

} // namespace
} // namespace pbr
