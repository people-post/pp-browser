// The 1:1 call-media control channel's wire messages, and what happens to a path a call left.
#include "lib/calls/CallMediaBundleLogic.h"
#include "lib/calls/CallMediaControlCodec.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CallMediaControlCodecTest, EachMessageRoundTrips) {
  auto hello = DecodeCallMediaControl(EncodeCallMediaHello("call:1", 3, /*offerer=*/true));
  ASSERT_TRUE(hello);
  EXPECT_EQ(hello->type, CallMediaControlType::Hello);
  EXPECT_EQ(hello->call_id, "call:1");
  EXPECT_EQ(hello->media_epoch, 3u);
  EXPECT_TRUE(hello->sender_offerer);
  EXPECT_FALSE(DecodeCallMediaControl(EncodeCallMediaHello("call:1", 3, false))->sender_offerer);

  auto nack = DecodeCallMediaControl(EncodeCallMediaHelloAck(false, "busy"));
  EXPECT_EQ(nack->type, CallMediaControlType::HelloAck);
  EXPECT_FALSE(nack->ok);
  EXPECT_EQ(nack->error, "busy");
  EXPECT_TRUE(DecodeCallMediaControl(EncodeCallMediaHelloAck(true))->ok);

  EXPECT_TRUE(DecodeCallMediaControl(EncodeCallMediaHeartbeat(true))->active);
  EXPECT_EQ(DecodeCallMediaControl(EncodeCallMediaHeartbeat(false))->type, CallMediaControlType::Heartbeat);

  auto migrate = DecodeCallMediaControl(EncodeCallMediaMigrate("call:1", 2, 7, /*standby_only=*/false));
  EXPECT_EQ(migrate->type, CallMediaControlType::Migrate);
  EXPECT_EQ(migrate->path_gen, 7u);
  EXPECT_EQ(migrate->media_epoch, 2u);
  EXPECT_EQ(DecodeCallMediaControl(EncodeCallMediaMigrate("call:1", 2, 7, true))->type,
            CallMediaControlType::PathAdd);
  auto ack = DecodeCallMediaControl(EncodeCallMediaMigrateAck(false, 7, "no link"));
  EXPECT_EQ(ack->type, CallMediaControlType::MigrateAck);
  EXPECT_EQ(ack->error, "no link");
  EXPECT_EQ(DecodeCallMediaControl(EncodeCallMediaPathRelease(false, 4))->type, CallMediaControlType::PathRelease);
  EXPECT_EQ(DecodeCallMediaControl(EncodeCallMediaPathRelease(true, 4))->type, CallMediaControlType::PathReleaseAck);
}

TEST(CallMediaControlCodecTest, DefaultsAndJunk) {
  EXPECT_FALSE(DecodeCallMediaControl("not json"));
  EXPECT_EQ(DecodeCallMediaControl(R"({"type":"later"})")->type, CallMediaControlType::Unknown);
  EXPECT_EQ(DecodeCallMediaControl(R"({"type":"hello","call_id":"c"})")->media_epoch, 1u) << "hello: epoch 1";
  EXPECT_EQ(DecodeCallMediaControl(R"({"type":"migrate","call_id":"c"})")->media_epoch, 0u);
}

TEST(CallMediaRetiringTest, ThePathACallLeft) {
  CallMediaRetiringInput in;
  in.drove_switch = true;
  in.can_send = true;
  EXPECT_EQ(DecideCallMediaRetiring(in), CallMediaRetiringAction::Keep) << "just switched";
  in.since_switch_ms = kCallMediaRetireAfterSwitchMs;
  EXPECT_EQ(DecideCallMediaRetiring(in), CallMediaRetiringAction::Keep) << "no media on the new path yet";
  in.rx_since_switch = true;
  EXPECT_EQ(DecideCallMediaRetiring(in), CallMediaRetiringAction::SendRelease);
  in.rx_since_switch = false;
  in.since_switch_ms = kCallMediaRetireAtMostMs;
  EXPECT_EQ(DecideCallMediaRetiring(in), CallMediaRetiringAction::SendRelease) << "a silent peer";
  in.release_sent = true;
  EXPECT_EQ(DecideCallMediaRetiring(in), CallMediaRetiringAction::Keep) << "once";
  in.drove_switch = false;
  in.release_sent = false;
  EXPECT_EQ(DecideCallMediaRetiring(in), CallMediaRetiringAction::Keep) << "the driver releases";
  in.since_switch_ms = kCallMediaRetireAbandonMs;
  EXPECT_EQ(DecideCallMediaRetiring(in), CallMediaRetiringAction::Drop) << "never acknowledged";
  in.since_switch_ms = 0;
  in.link_lost = true;
  EXPECT_EQ(DecideCallMediaRetiring(in), CallMediaRetiringAction::Drop);
}

} // namespace
} // namespace pbr
