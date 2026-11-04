#include "domain/call_media/CallMediaFrameCrypto.h"
#include "mesh/l4/shared/MediaFrame.h"
#include "mesh/types/MediaChannel.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

const mesh::ByteVector kKey(32, 0x42);
const std::vector<uint8_t> kOpus = {1, 2, 3, 4, 5};

mesh::MediaDataFrame Sealed(const std::string& context, uint16_t channel = mesh::kMediaChannelAudio) {
  mesh::MediaDataFrame f;
  f.stream_id = 77;
  f.channel_id = channel;
  f.channel_type = mesh::MediaChannelTypeFor(channel);
  f.seq = 9;
  f.instance = 0x1234;
  EXPECT_TRUE(mesh::SealMediaFrame(kKey, context, 1, f, kOpus));
  return f;
}

// A029 H2: one frame for every path; the context keeps a frame to the feature, path and session it was sealed for.
TEST(CallMediaFrameCryptoTest, AFrameOpensOnlyUnderItsOwnPathAndCall) {
  const auto sfu = Sealed(CallMediaSfuContext("call:1"));
  auto opened = mesh::OpenMediaFrame(kKey, CallMediaSfuContext("call:1"), 1, sfu);
  ASSERT_TRUE(opened) << opened.error().message;
  EXPECT_EQ(*opened, kOpus);
  EXPECT_FALSE(mesh::OpenMediaFrame(kKey, CallMediaDirectContext("call:1"), 1, sfu)) << "the 1:1 path";
  EXPECT_FALSE(mesh::OpenMediaFrame(kKey, CallMediaSfuContext("call:2"), 1, sfu)) << "another call";

  const auto direct = Sealed(CallMediaDirectContext("call:1"));
  EXPECT_TRUE(mesh::OpenMediaFrame(kKey, CallMediaDirectContext("call:1"), 1, direct));
  EXPECT_FALSE(mesh::OpenMediaFrame(kKey, CallMediaSfuContext("call:1"), 1, direct)) << "a hop's fan-out";
}

// A group call seals once: every participant holding the epoch key opens the same bytes.
TEST(CallMediaFrameCryptoTest, OneSealOpensForEveryParticipantWithTheKey) {
  const auto video = Sealed(CallMediaSfuContext("call:g"), mesh::VideoChannel(mesh::kDefaultVideoLevel));
  const auto wire = mesh::EncodeMediaDataFrame(video);
  for (int peer = 0; peer < 2; ++peer) {
    auto frame = mesh::DecodeMediaDataFrame(wire);
    ASSERT_TRUE(frame);
    EXPECT_TRUE(mesh::OpenMediaFrame(kKey, CallMediaSfuContext("call:g"), 1, *frame));
  }
  EXPECT_FALSE(mesh::OpenMediaFrame(mesh::ByteVector(32, 0x24), CallMediaSfuContext("call:g"), 1, video));
}

} // namespace
} // namespace pbr
