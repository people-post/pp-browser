// DEV/PLAN-call-frame-cost.md step 2, the UI side: a frame is copied out of the engine and handed to the tiles only
// when it is not the one the tiles already hold. No picture, the same picture and a new picture are three cases.
#include "gui/CallVideoTiles.h"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

using FrameRead = IMediaView::FrameRead;

/** The engine's reads with the engine's rules (seq compared by inequality), frames set by the test. */
struct FakeMedia {
  std::optional<uint64_t> local;
  std::optional<uint64_t> remote;
  std::map<uint32_t, uint64_t> streams;
  int copies = 0;

  FrameRead Read(const std::optional<uint64_t>& seq, const uint64_t last_seq, IMediaView::VideoTileFrame& out) {
    if (!seq) {
      return FrameRead::None;
    }
    if (*seq == last_seq) {
      return FrameRead::Same;
    }
    ++copies;
    out.width = 2;
    out.height = 2;
    out.seq = *seq;
    out.rgba.assign(16, 0x20);
    return FrameRead::New;
  }
  FrameRead CopyLocalVideoFrameIfNew(uint64_t last_seq, IMediaView::VideoTileFrame& out) {
    return Read(local, last_seq, out);
  }
  FrameRead CopyRemoteVideoFrameIfNew(uint64_t last_seq, IMediaView::VideoTileFrame& out) {
    return Read(remote, last_seq, out);
  }
  FrameRead CopyRemoteVideoFrameForStreamIfNew(uint32_t stream, uint64_t last_seq, IMediaView::VideoTileFrame& out) {
    auto it = streams.find(stream);
    return Read(it == streams.end() ? std::nullopt : std::optional<uint64_t>(it->second), last_seq, out);
  }
  bool HasRemoteVideoForStream(uint32_t stream) const { return streams.count(stream) > 0; }
};

CallRosterParticipantState Row(const uint32_t stream, const bool local = false) {
  CallRosterParticipantState row;
  row.is_local = local;
  row.stream_id = std::to_string(stream).c_str();
  return row;
}

class CallVideoTilesTest : public testing::Test {
protected:
  void SetUp() override { tiles.Clear(); }
  void TearDown() override { tiles.Clear(); }
  CallVideoTileRenderer& tiles = CallVideoTileRenderer::Instance();
  FakeMedia media;
};

TEST_F(CallVideoTilesTest, NoPictureShowsNoSelfView) {
  const CallVideoTilesShown shown = SubmitCallVideoTiles(media, tiles, /*remote_video=*/true);
  EXPECT_FALSE(shown.local_preview);
  EXPECT_FALSE(shown.self_new);
  EXPECT_FALSE(shown.peer_new);
  EXPECT_EQ(tiles.HeldSeq(CallVideoTileKind::Local), 0u);
  EXPECT_EQ(tiles.HeldSeq(CallVideoTileKind::Remote), 0u);
}

TEST_F(CallVideoTilesTest, TheSamePictureIsNeitherCopiedNorSubmittedAndTheSelfViewStays) {
  media.local = 5;
  media.remote = 9;
  CallVideoTilesShown shown = SubmitCallVideoTiles(media, tiles, true);
  EXPECT_TRUE(shown.local_preview);
  EXPECT_TRUE(shown.self_new);
  EXPECT_TRUE(shown.peer_new);
  EXPECT_EQ(media.copies, 2);
  EXPECT_EQ(tiles.HeldSeq(CallVideoTileKind::Local), 5u);
  EXPECT_EQ(tiles.HeldSeq(CallVideoTileKind::Remote), 9u);

  shown = SubmitCallVideoTiles(media, tiles, true); // the next frame: nothing new arrived
  EXPECT_EQ(media.copies, 2) << "nothing copied";
  EXPECT_TRUE(shown.local_preview) << "the self-view (and so the stage) stays up";
  EXPECT_FALSE(shown.self_new) << "the metrics hear: not a new frame";
  EXPECT_FALSE(shown.peer_new);
  EXPECT_EQ(tiles.HeldSeq(CallVideoTileKind::Local), 5u) << "the tiles keep drawing the frame they hold";
  EXPECT_EQ(tiles.HeldSeq(CallVideoTileKind::Remote), 9u);

  media.remote = 10;
  shown = SubmitCallVideoTiles(media, tiles, true);
  EXPECT_TRUE(shown.peer_new);
  EXPECT_FALSE(shown.self_new);
  EXPECT_EQ(media.copies, 3);
  EXPECT_EQ(tiles.HeldSeq(CallVideoTileKind::Remote), 10u);
}

TEST_F(CallVideoTilesTest, NoRemoteVideoReadsNoPeerPicture) {
  media.remote = 9;
  const CallVideoTilesShown shown = SubmitCallVideoTiles(media, tiles, /*remote_video=*/false);
  EXPECT_FALSE(shown.peer_new);
  EXPECT_EQ(media.copies, 0);
}

TEST_F(CallVideoTilesTest, APeerPictureClearedFromTheTilesIsSubmittedAgainWithTheSameNumber) {
  media.remote = 9;
  SubmitCallVideoTiles(media, tiles, true);
  tiles.ClearRemote(); // remote video went (a stall, their camera off) and came back with the same frame
  const CallVideoTilesShown shown = SubmitCallVideoTiles(media, tiles, true);
  EXPECT_TRUE(shown.peer_new);
  EXPECT_EQ(tiles.HeldSeq(CallVideoTileKind::Remote), 9u);
}

TEST_F(CallVideoTilesTest, TheFirstFrameOfTheNextCallIsSubmittedEvenWithTheSameNumber) {
  media.local = 1;
  media.remote = 1;
  media.streams = {{101, 1}};
  std::vector<CallRosterParticipantState> roster = {Row(0, true), Row(101)};
  SubmitCallVideoTiles(media, tiles, true);
  SubmitCallRosterTiles(media, tiles, roster);
  tiles.Clear(); // the call ended (CallController::ClearInCall); the next call's frames are numbered from 1 again
  const int before = media.copies;
  const CallVideoTilesShown shown = SubmitCallVideoTiles(media, tiles, true);
  SubmitCallRosterTiles(media, tiles, roster);
  EXPECT_TRUE(shown.self_new);
  EXPECT_TRUE(shown.peer_new);
  EXPECT_EQ(media.copies, before + 3);
  EXPECT_EQ(tiles.HeldSeq(CallVideoTileKind::Peer, 101), 1u);
}

TEST_F(CallVideoTilesTest, RosterTilesCopyOnlyNewFramesPerStream) {
  media.streams = {{101, 4}, {102, 7}};
  std::vector<CallRosterParticipantState> roster = {Row(0, true), Row(101), Row(102), Row(103)};
  std::vector<uint32_t> live = SubmitCallRosterTiles(media, tiles, roster);
  EXPECT_EQ(live, (std::vector<uint32_t>{101, 102}));
  EXPECT_FALSE(roster[0].has_remote_video);
  EXPECT_TRUE(roster[1].has_remote_video);
  EXPECT_TRUE(roster[2].has_remote_video);
  EXPECT_FALSE(roster[3].has_remote_video) << "no picture on that stream";
  EXPECT_EQ(media.copies, 2);

  media.streams[102] = 8;
  live = SubmitCallRosterTiles(media, tiles, roster);
  EXPECT_EQ(media.copies, 3) << "only stream 102 had a new frame";
  EXPECT_TRUE(roster[1].has_remote_video) << "a stream with no new frame is still on screen";
  EXPECT_EQ(tiles.HeldSeq(CallVideoTileKind::Peer, 101), 4u);
  EXPECT_EQ(tiles.HeldSeq(CallVideoTileKind::Peer, 102), 8u);
}

TEST_F(CallVideoTilesTest, SomeoneNewOnTheSameStreamWithTheSameNumberIsStillShown) {
  media.streams = {{101, 4}};
  std::vector<CallRosterParticipantState> roster = {Row(0, true), Row(101)};
  SubmitCallRosterTiles(media, tiles, roster);
  ASSERT_EQ(tiles.HeldSeq(CallVideoTileKind::Peer, 101), 4u);
  // They leave: the row goes and the tiles let go of the stream (RetainPeers).
  roster = {Row(0, true)};
  media.streams.clear();
  SubmitCallRosterTiles(media, tiles, roster);
  EXPECT_EQ(tiles.HeldSeq(CallVideoTileKind::Peer, 101), 0u);
  // Someone joins on the same stream id and their frame happens to carry the same number.
  roster = {Row(0, true), Row(101)};
  media.streams = {{101, 4}};
  const int before = media.copies;
  SubmitCallRosterTiles(media, tiles, roster);
  EXPECT_EQ(media.copies, before + 1);
  EXPECT_EQ(tiles.HeldSeq(CallVideoTileKind::Peer, 101), 4u);
  EXPECT_TRUE(roster[1].has_remote_video);
}

} // namespace
} // namespace pbr
