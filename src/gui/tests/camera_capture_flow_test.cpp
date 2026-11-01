#include "gui/chat/CameraCaptureFlow.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

// Records what the capture result asked the chat to do; `release` is the injected release_pick_.
struct Recorder {
  std::vector<std::string> released;
  std::vector<std::string> asked_picture;
  std::vector<std::string> sent_video;
  std::vector<CaptureNotice> notices;
  uint64_t size = 1000;
  AttachmentLimits limits = DefaultAttachmentLimits();

  CaptureFlowPorts Ports() {
    CaptureFlowPorts ports;
    ports.release = [this](const std::string& path) { released.push_back(path); };
    ports.ask_picture = [this](std::string path) { asked_picture.push_back(std::move(path)); };
    ports.send_video = [this](std::string path) { sent_video.push_back(std::move(path)); };
    ports.say = [this](const CaptureNotice notice) { notices.push_back(notice); };
    ports.file_size = [this](const std::string&) { return size; };
    ports.limits = [this]() { return limits; };
    return ports;
  }
  bool Silent() const { return released.empty() && asked_picture.empty() && sent_video.empty() && notices.empty(); }
};

TEST(CameraCaptureFlowTest, CancelledSaysAndDoesNothing) {
  Recorder rec;
  HandleCaptureResult(rec.Ports(), CameraCaptureOutcome::Cancelled, {});
  EXPECT_TRUE(rec.Silent());
}

TEST(CameraCaptureFlowTest, UnavailableAndUnprocessableEachTellTheirOwnReason) {
  Recorder rec;
  HandleCaptureResult(rec.Ports(), CameraCaptureOutcome::Unavailable, {});
  HandleCaptureResult(rec.Ports(), CameraCaptureOutcome::Unprocessable, {});
  ASSERT_EQ(rec.notices.size(), 2u);
  EXPECT_EQ(rec.notices[0], CaptureNotice::CameraUnavailable);
  EXPECT_EQ(rec.notices[1], CaptureNotice::VideoUnprocessable);
  EXPECT_TRUE(rec.asked_picture.empty());
  EXPECT_TRUE(rec.sent_video.empty());
}

TEST(CameraCaptureFlowTest, APictureGoesToTheSendChoiceAndIsNotReleasedYet) {
  Recorder rec;
  HandleCaptureResult(rec.Ports(), CameraCaptureOutcome::Picked, {"/cache/capture/IMG.jpg"});
  ASSERT_EQ(rec.asked_picture.size(), 1u);
  EXPECT_EQ(rec.asked_picture[0], "/cache/capture/IMG.jpg");
  EXPECT_TRUE(rec.released.empty()) << "the choice (or the upload) owns the release";
  EXPECT_TRUE(rec.sent_video.empty());
  EXPECT_TRUE(rec.notices.empty());
}

TEST(CameraCaptureFlowTest, AVideoWithinTheLimitIsSentAsItIs) {
  Recorder rec;
  rec.size = rec.limits.max_video_bytes;
  HandleCaptureResult(rec.Ports(), CameraCaptureOutcome::Picked, {"/cache/capture/VID.mp4"});
  ASSERT_EQ(rec.sent_video.size(), 1u);
  EXPECT_EQ(rec.sent_video[0], "/cache/capture/VID.mp4");
  EXPECT_TRUE(rec.released.empty());
  EXPECT_TRUE(rec.notices.empty());
  EXPECT_TRUE(rec.asked_picture.empty());
}

TEST(CameraCaptureFlowTest, AVideoOverTheLimitIsReleasedOnceAndTheyAreToldToRecordShorter) {
  Recorder rec;
  rec.size = rec.limits.max_video_bytes + 1;
  HandleCaptureResult(rec.Ports(), CameraCaptureOutcome::Picked, {"/cache/capture/VID.mov"});
  ASSERT_EQ(rec.released.size(), 1u);
  EXPECT_EQ(rec.released[0], "/cache/capture/VID.mov");
  ASSERT_EQ(rec.notices.size(), 1u);
  EXPECT_EQ(rec.notices[0], CaptureNotice::VideoTooLarge);
  EXPECT_TRUE(rec.sent_video.empty());
}

TEST(CameraCaptureFlowTest, PickedWithoutAFileIsNotSilent) {
  Recorder rec;
  HandleCaptureResult(rec.Ports(), CameraCaptureOutcome::Picked, {});
  ASSERT_EQ(rec.notices.size(), 1u);
  EXPECT_EQ(rec.notices[0], CaptureNotice::CameraUnavailable);
}

struct ChoiceRecorder {
  std::vector<std::pair<std::string, bool>> sent;
  std::vector<std::string> released;
  std::vector<ContextMenuAction> Actions() {
    return PictureChoiceActions(
        "/cache/capture/IMG.jpg", [this](const std::string& path, bool original) { sent.emplace_back(path, original); },
        [this](const std::string& path) { released.push_back(path); }, "Send", "Send original", "Cancel");
  }
};

TEST(PictureChoiceActionsTest, ThreeEntriesInOrder) {
  ChoiceRecorder rec;
  const auto actions = rec.Actions();
  ASSERT_EQ(actions.size(), 3u);
  EXPECT_EQ(actions[0].label, "Send");
  EXPECT_EQ(actions[1].label, "Send original");
  EXPECT_EQ(actions[2].label, "Cancel");
}

TEST(PictureChoiceActionsTest, CancelReleasesTheFileOnceAndSendsNothing) {
  ChoiceRecorder rec;
  rec.Actions()[2].run();
  EXPECT_TRUE(rec.sent.empty());
  ASSERT_EQ(rec.released.size(), 1u);
  EXPECT_EQ(rec.released[0], "/cache/capture/IMG.jpg");
}

TEST(PictureChoiceActionsTest, SendAndSendOriginalHandTheFileOnWithoutReleasingIt) {
  ChoiceRecorder rec;
  auto actions = rec.Actions();
  actions[0].run();
  actions[1].run();
  ASSERT_EQ(rec.sent.size(), 2u);
  EXPECT_FALSE(rec.sent[0].second);
  EXPECT_TRUE(rec.sent[1].second);
  EXPECT_EQ(rec.sent[0].first, "/cache/capture/IMG.jpg");
  EXPECT_TRUE(rec.released.empty()) << "StartAttachmentUpload releases it";
}

} // namespace
} // namespace pbr
