// Pictures picked together go out one at a time with the typed line after them (DEV/PLAN-picture-send-preview.md):
// the queue's choosing, its one-at-a-time sending, the send pace wait on a fake clock, every way it ends (the plan's
// "文字去向" table), the files it releases, and the once-only end of one send.
#include "feature/presenters/PictureSendQueue.h"

#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr const char* kChat = "chat-a";

/** What the fake ports saw, in order, and the fake world they answer from. */
struct Fake {
  std::vector<std::string> events;
  /** The item being sent now (0: none); a second offer while one is out is recorded as a violation. */
  uint64_t in_flight = 0;
  int offered_while_busy = 0;
  std::vector<std::pair<std::string, bool>> sent_items; // path, original
  std::vector<PictureSendKind> sent_kinds;
  std::map<std::string, int> released;
  std::vector<std::string> removed;
  std::vector<int64_t> ticks_asked;
  int64_t now = 1'000'000;
  bool offline = false;
  bool text_goes = true;
  int done_calls = 0;
  std::optional<PictureSendReport> report;
  /** Answers an offer at once (a refusal) when set. */
  std::optional<AttachmentSendOutcome> answer_at_once;
  PictureSendQueue* queue = nullptr;
};

PictureSendPorts PortsOf(Fake& fake) {
  PictureSendPorts ports;
  ports.send_item = [&fake](const PictureSendItem& item, const bool original) {
    if (fake.in_flight != 0) {
      ++fake.offered_while_busy;
    }
    fake.in_flight = item.id;
    fake.sent_items.emplace_back(item.path, original);
    fake.sent_kinds.push_back(item.kind);
    fake.events.push_back("send " + item.path);
    if (fake.answer_at_once && fake.queue) {
      const uint64_t id = fake.in_flight;
      fake.in_flight = 0;
      fake.queue->OnItemFinished(id, *fake.answer_at_once);
    }
  };
  ports.send_text = [&fake](const std::string& thread, const std::string& text) {
    fake.events.push_back("text " + thread + " " + text);
    return fake.text_goes;
  };
  ports.put_back_text = [&fake](const std::string& thread, const std::string& text) {
    fake.events.push_back("put back " + thread + " " + text);
  };
  ports.release = [&fake](const std::string& path) {
    ++fake.released[path];
    fake.events.push_back("release " + path);
  };
  ports.remove_file = [&fake](const std::string& path) { fake.removed.push_back(path); };
  ports.now_ms = [&fake]() { return fake.now; };
  ports.schedule_tick = [&fake](const int64_t delay) { fake.ticks_asked.push_back(delay); };
  ports.offline = [&fake]() { return fake.offline; };
  ports.done = [&fake](const PictureSendReport& report) {
    ++fake.done_calls;
    fake.report = report;
    fake.events.push_back("done");
  };
  return ports;
}

class PictureSendQueueTest : public ::testing::Test {
protected:
  PictureSendQueue& Make(AttachmentLimits limits = DefaultAttachmentLimits()) {
    queue_.emplace(kChat, limits, PortsOf(fake_));
    fake_.queue = &*queue_;
    return *queue_;
  }
  /** Three pictures p1..p3 picked in order. */
  PictureSendQueue& MakeThree() {
    PictureSendQueue& queue = Make();
    for (const char* path : {"p1.jpg", "p2.jpg", "p3.jpg"}) {
      EXPECT_TRUE(queue.Add(path, PictureSendKind::Picture, 1000).has_value());
    }
    return queue;
  }
  /** The item in flight ends with `outcome`. */
  void Finish(AttachmentSendOutcome outcome) {
    ASSERT_NE(fake_.in_flight, 0u) << "nothing is being sent";
    const uint64_t id = fake_.in_flight;
    fake_.in_flight = 0;
    queue_->OnItemFinished(id, outcome);
  }
  size_t Count(const std::string& prefix) const {
    return static_cast<size_t>(std::count_if(fake_.events.begin(), fake_.events.end(),
                                             [&prefix](const std::string& e) { return e.rfind(prefix, 0) == 0; }));
  }
  size_t IndexOf(const std::string& event) const {
    return static_cast<size_t>(std::find(fake_.events.begin(), fake_.events.end(), event) - fake_.events.begin());
  }
  /** Every picked copy was released exactly once. */
  void ExpectEachReleasedOnce(const std::vector<std::string>& paths) const {
    for (const std::string& path : paths) {
      auto it = fake_.released.find(path);
      ASSERT_NE(it, fake_.released.end()) << path << " never released";
      EXPECT_EQ(it->second, 1) << path;
    }
  }

  Fake fake_;
  std::optional<PictureSendQueue> queue_;
};

// --- The outcome of one send and its once-only end ---

TEST(ClassifyUploadOutcomeTest, SentWinsThenCancelThenStagedThenPace) {
  EXPECT_EQ(ClassifyUploadOutcome(true, true, false, false), AttachmentSendOutcome::Sent) << "a late cancel";
  EXPECT_EQ(ClassifyUploadOutcome(false, true, true, true), AttachmentSendOutcome::Cancelled);
  EXPECT_EQ(ClassifyUploadOutcome(false, false, true, true), AttachmentSendOutcome::FailedStaged)
      << "a server 429 after staging keeps its Retry";
  EXPECT_EQ(ClassifyUploadOutcome(false, false, false, true), AttachmentSendOutcome::RateLimited);
  EXPECT_EQ(ClassifyUploadOutcome(false, false, false, false), AttachmentSendOutcome::FailedNotStaged);
}

TEST(ClassifyUploadOutcomeTest, OnlyTheBlobPaceErrorIsRateLimited) {
  EXPECT_TRUE(IsAttachmentRateLimited(AppError::Blob(Err::Blob::RateLimited, "pace")));
  EXPECT_FALSE(IsAttachmentRateLimited(AppError::Blob(Err::Blob::QuotaExceeded, "full")));
  EXPECT_FALSE(IsAttachmentRateLimited(AppError::Auth(Err::Auth::RateLimited, "auth pace")));
  EXPECT_FALSE(IsAttachmentRateLimited(Error("plain")));
}

TEST(AttachmentSendOnceTest, OnlyTheFirstOfTwoCallbacksCounts) {
  std::vector<AttachmentSendOutcome> got;
  const AttachmentSendOnce once([&got](AttachmentSendOutcome outcome) { got.push_back(outcome); });
  const AttachmentSendOnce copy = once; // the closures hold copies
  copy.Fire(AttachmentSendOutcome::Sent);
  once.Fire(AttachmentSendOutcome::FailedNotStaged);
  copy.Fire(AttachmentSendOutcome::Cancelled);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got.front(), AttachmentSendOutcome::Sent);
  EXPECT_TRUE(once.Fired());
}

TEST(AttachmentSendOnceTest, ADroppedCallbackStillReportsOnceWhenTheLastCopyGoes) {
  std::vector<AttachmentSendOutcome> got;
  {
    const AttachmentSendOnce once([&got](AttachmentSendOutcome outcome) { got.push_back(outcome); });
    const AttachmentSendOnce copy = once;
  }
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got.front(), AttachmentSendOutcome::FailedNotStaged);
  {
    const AttachmentSendOnce fired([&got](AttachmentSendOutcome outcome) { got.push_back(outcome); });
    fired.Fire(AttachmentSendOutcome::Sent);
  }
  EXPECT_EQ(got.size(), 2u) << "a fired one says nothing more when it goes";
  AttachmentSendOnce().Fire(AttachmentSendOutcome::Sent); // no callback: nothing happens
}

// --- Choosing ---

TEST_F(PictureSendQueueTest, AddingMoreThanNineReleasesTheExtra) {
  PictureSendQueue& queue = Make();
  for (int i = 0; i < 9; ++i) {
    ASSERT_TRUE(queue.Add("p" + std::to_string(i), PictureSendKind::Picture, 10).has_value());
  }
  EXPECT_FALSE(queue.Add("p9", PictureSendKind::Picture, 10).has_value());
  EXPECT_EQ(queue.Items().size(), 9u);
  ExpectEachReleasedOnce({"p9"});
}

TEST_F(PictureSendQueueTest, RemovingAnItemReleasesItsFilesAndKeepsTheOrder) {
  PictureSendQueue& queue = MakeThree();
  const uint64_t second = queue.Items()[1].id;
  ASSERT_TRUE(queue.SetPreview(second, "pv2", "th2"));
  EXPECT_TRUE(queue.Remove(second));
  ASSERT_EQ(queue.Items().size(), 2u);
  EXPECT_EQ(queue.Items()[0].path, "p1.jpg");
  EXPECT_EQ(queue.Items()[1].path, "p3.jpg");
  ExpectEachReleasedOnce({"p2.jpg"});
  EXPECT_EQ(fake_.removed, (std::vector<std::string>{"pv2", "th2"}));
  EXPECT_FALSE(queue.Remove(second)) << "already gone";
  EXPECT_FALSE(queue.SetPreview(second, "late", "late")) << "a late preview is the caller's to delete";
  EXPECT_EQ(queue.SelectedBytes(), 2000u);
  // Removing every item leaves nothing to send.
  for (const uint64_t id : {queue.Items()[0].id, queue.Items()[1].id}) {
    EXPECT_TRUE(queue.Remove(id));
  }
  EXPECT_FALSE(queue.Start());
}

TEST_F(PictureSendQueueTest, NothingIsRemovedOnceSendingBegan) {
  PictureSendQueue& queue = MakeThree();
  ASSERT_TRUE(queue.Start());
  EXPECT_FALSE(queue.Remove(queue.Items()[2].id));
}

TEST_F(PictureSendQueueTest, OriginalDecidesHowPicturesGoAndVideosAreUntouched) {
  PictureSendQueue& queue = Make();
  (void)queue.Add("a.jpg", PictureSendKind::Picture, 10);
  (void)queue.Add("b.mov", PictureSendKind::Video, 10);
  queue.SetOriginal(true);
  ASSERT_TRUE(queue.Start());
  Finish(AttachmentSendOutcome::Sent);
  Finish(AttachmentSendOutcome::Sent);
  ASSERT_EQ(fake_.sent_items.size(), 2u);
  EXPECT_TRUE(fake_.sent_items[0].second);
  EXPECT_EQ(fake_.sent_items[1].first, "b.mov");
}

TEST_F(PictureSendQueueTest, OverLimitItemsAreMarkedSkippedAndCounted) {
  AttachmentLimits limits = DefaultAttachmentLimits();
  limits.max_video_bytes = 100;
  limits.max_original_image_bytes = 100;
  limits.max_image_pixels = 1000;
  PictureSendQueue& queue = Make(limits);
  (void)queue.Add("small.jpg", PictureSendKind::Picture, 50, 500);
  (void)queue.Add("huge.mov", PictureSendKind::Video, 500);
  (void)queue.Add("big.jpg", PictureSendKind::Picture, 500, 500);  // too big only as it is
  (void)queue.Add("wide.jpg", PictureSendKind::Picture, 50, 5000); // too many pixels only to shrink
  EXPECT_EQ(queue.SendableCount(), 2u) << "shrunk: the video and the wide picture";
  queue.SetOriginal(true);
  EXPECT_EQ(queue.SendableCount(), 2u) << "as it is: the video and the big picture";
  EXPECT_TRUE(queue.Items()[2].too_large);
  EXPECT_FALSE(queue.Items()[3].too_large);
  EXPECT_EQ(queue.SelectedBytes(), 1100u) << "the size beside 原图 counts every item";
  queue.SetText("hi");
  ASSERT_TRUE(queue.Start());
  ExpectEachReleasedOnce({"huge.mov", "big.jpg"}); // skipped at once, never offered
  Finish(AttachmentSendOutcome::Sent);
  Finish(AttachmentSendOutcome::Sent);
  EXPECT_EQ(fake_.sent_items.size(), 2u);
  ASSERT_TRUE(fake_.report);
  EXPECT_EQ(fake_.report->too_large, 2);
  EXPECT_EQ(fake_.report->NotSent(), 2);
  EXPECT_EQ(fake_.report->text, PictureTextFate::Sent);
  ExpectEachReleasedOnce({"small.jpg", "huge.mov", "big.jpg", "wide.jpg"});
}

TEST_F(PictureSendQueueTest, StartingDeletesThePreviewFiles) {
  PictureSendQueue& queue = MakeThree();
  ASSERT_TRUE(queue.SetPreview(queue.Items()[0].id, "pv1", "th1"));
  ASSERT_TRUE(queue.Start());
  EXPECT_EQ(fake_.removed, (std::vector<std::string>{"pv1", "th1"}));
  EXPECT_FALSE(queue.SetPreview(queue.Items()[1].id, "pv2", "th2"));
}

// --- Files beside the pictures (DEV/PLAN-desktop-drop-paste.md, step B) ---

TEST_F(PictureSendQueueTest, AFileKeepsItsNameAndTheOwnedFlagItWasGiven) {
  PictureSendQueue& queue = Make();
  ASSERT_TRUE(queue.Add("/some/dir/report.pdf", PictureSendKind::File, 10).has_value());
  ASSERT_TRUE(queue.Add("/tmp/pasted/x.png", PictureSendKind::Picture, 10, 0, "Pasted image 1.png", true).has_value());
  EXPECT_EQ(queue.Items()[0].display_name, "report.pdf") << "the path's file name";
  EXPECT_FALSE(queue.Items()[0].owned);
  EXPECT_EQ(queue.Items()[1].display_name, "Pasted image 1.png") << "a name the caller gave";
  EXPECT_TRUE(queue.Items()[1].owned);
}

TEST_F(PictureSendQueueTest, FilesGoInOrderAmongPicturesWhateverTheOriginalChoiceAndTheTextLast) {
  PictureSendQueue& queue = Make();
  (void)queue.Add("a.jpg", PictureSendKind::Picture, 10);
  (void)queue.Add("b.pdf", PictureSendKind::File, 10);
  (void)queue.Add("c.mp4", PictureSendKind::Video, 10);
  (void)queue.Add("d.zip", PictureSendKind::File, 10);
  queue.SetOriginal(true);
  queue.SetText("see");
  ASSERT_TRUE(queue.Start());
  for (int i = 0; i < 4; ++i) {
    Finish(AttachmentSendOutcome::Sent);
  }
  EXPECT_EQ(fake_.events,
            (std::vector<std::string>{"send a.jpg", "release a.jpg", "send b.pdf", "release b.pdf", "send c.mp4",
                                      "release c.mp4", "send d.zip", "release d.zip", "text chat-a see", "done"}));
  EXPECT_EQ(fake_.sent_kinds, (std::vector<PictureSendKind>{PictureSendKind::Picture, PictureSendKind::File,
                                                            PictureSendKind::Video, PictureSendKind::File}));
  EXPECT_EQ(fake_.offered_while_busy, 0);
  EXPECT_EQ(fake_.report->sent, 4);
}

TEST_F(PictureSendQueueTest, AFileIsOverTheLimitByItsOwnByteLimitNotByThePictureOnes) {
  AttachmentLimits limits = DefaultAttachmentLimits();
  limits.max_file_bytes = 100;
  limits.max_original_image_bytes = 1000;
  limits.max_image_pixels = 10;
  PictureSendQueue& queue = Make(limits);
  (void)queue.Add("small.pdf", PictureSendKind::File, 100, 5000); // pixels mean nothing to a file
  (void)queue.Add("big.pdf", PictureSendKind::File, 101);
  for (const bool original : {false, true, false}) {
    queue.SetOriginal(original);
    EXPECT_FALSE(queue.Items()[0].too_large) << "original: " << original;
    EXPECT_TRUE(queue.Items()[1].too_large) << "original: " << original;
  }
  EXPECT_EQ(queue.SendableCount(), 1u);
}

TEST_F(PictureSendQueueTest, AFileOver50MbIsMarkedTooLargeSkippedAndTheRestStillGo) {
  PictureSendQueue& queue = Make();
  (void)queue.Add("ok.pdf", PictureSendKind::File, kDefaultAttachmentSendMaxBytes);
  (void)queue.Add("huge.zip", PictureSendKind::File, kDefaultAttachmentSendMaxBytes + 1);
  EXPECT_FALSE(queue.Items()[0].too_large);
  EXPECT_TRUE(queue.Items()[1].too_large);
  queue.SetText("x");
  ASSERT_TRUE(queue.Start());
  ExpectEachReleasedOnce({"huge.zip"});
  Finish(AttachmentSendOutcome::Sent);
  EXPECT_EQ(fake_.sent_items.size(), 1u);
  EXPECT_EQ(fake_.report->too_large, 1);
  EXPECT_EQ(fake_.report->text, PictureTextFate::Sent);
}

// --- Sending one at a time, the text last ---

TEST_F(PictureSendQueueTest, NormalFinishSendsInOrderOneAtATimeThenTheText) {
  PictureSendQueue& queue = MakeThree();
  queue.SetText("look");
  ASSERT_TRUE(queue.Start());
  EXPECT_EQ(Count("send "), 1u) << "only the first until it ends";
  Finish(AttachmentSendOutcome::Sent);
  EXPECT_EQ(Count("text "), 0u);
  Finish(AttachmentSendOutcome::Sent);
  EXPECT_EQ(Count("text "), 0u);
  EXPECT_EQ(queue.Progress().number, 3u);
  EXPECT_EQ(queue.Progress().of, 3u);
  Finish(AttachmentSendOutcome::Sent);
  EXPECT_EQ(fake_.events, (std::vector<std::string>{"send p1.jpg", "release p1.jpg", "send p2.jpg", "release p2.jpg",
                                                    "send p3.jpg", "release p3.jpg", "text chat-a look", "done"}));
  EXPECT_EQ(fake_.offered_while_busy, 0);
  ASSERT_TRUE(fake_.report);
  EXPECT_EQ(fake_.report->exit, PictureSendExit::Finished);
  EXPECT_EQ(fake_.report->sent, 3);
  EXPECT_EQ(fake_.report->NotSent(), 0);
  EXPECT_TRUE(queue.Settled());
  EXPECT_FALSE(queue.Active());
}

TEST_F(PictureSendQueueTest, NormalFinishWithFailuresStillSendsTheTextAndCountsThem) {
  PictureSendQueue& queue = MakeThree();
  queue.SetText("look");
  ASSERT_TRUE(queue.Start());
  Finish(AttachmentSendOutcome::FailedStaged); // its bubble stays with Retry
  Finish(AttachmentSendOutcome::FailedNotStaged);
  Finish(AttachmentSendOutcome::Sent);
  ASSERT_TRUE(fake_.report);
  EXPECT_EQ(fake_.report->exit, PictureSendExit::Finished);
  EXPECT_EQ(fake_.report->failed_staged, 1);
  EXPECT_EQ(fake_.report->failed_not_staged, 1);
  EXPECT_EQ(fake_.report->NotSent(), 2);
  EXPECT_EQ(fake_.report->text, PictureTextFate::Sent);
  EXPECT_GT(IndexOf("text chat-a look"), IndexOf("send p3.jpg"));
  ExpectEachReleasedOnce({"p1.jpg", "p2.jpg", "p3.jpg"});
}

TEST_F(PictureSendQueueTest, NoTextNoTextMessage) {
  PictureSendQueue& queue = MakeThree();
  ASSERT_TRUE(queue.Start());
  Finish(AttachmentSendOutcome::Sent);
  Finish(AttachmentSendOutcome::Sent);
  Finish(AttachmentSendOutcome::Sent);
  EXPECT_EQ(Count("text "), 0u);
  EXPECT_EQ(fake_.report->text, PictureTextFate::None);
}

TEST_F(PictureSendQueueTest, TextThatCannotGoIsPutBackNotLost) {
  PictureSendQueue& queue = MakeThree();
  queue.SetText("look");
  fake_.text_goes = false;
  ASSERT_TRUE(queue.Start());
  Finish(AttachmentSendOutcome::Sent);
  Finish(AttachmentSendOutcome::Sent);
  Finish(AttachmentSendOutcome::Sent);
  EXPECT_EQ(fake_.report->text, PictureTextFate::PutBack);
  EXPECT_LT(IndexOf("text chat-a look"), IndexOf("put back chat-a look"));
}

TEST_F(PictureSendQueueTest, AnItemCancelledOnItsBubbleDoesNotStopTheRest) {
  PictureSendQueue& queue = MakeThree();
  ASSERT_TRUE(queue.Start());
  Finish(AttachmentSendOutcome::Cancelled);
  Finish(AttachmentSendOutcome::Sent);
  Finish(AttachmentSendOutcome::Sent);
  EXPECT_EQ(fake_.report->exit, PictureSendExit::Finished);
  EXPECT_EQ(fake_.report->cancelled, 1);
}

// --- The send pace (fake clock, no sleeps) ---

TEST_F(PictureSendQueueTest, RateLimitedWaitsTheWindowThenSendsTheSameItemAgain) {
  PictureSendQueue& queue = MakeThree();
  queue.SetText("look");
  ASSERT_TRUE(queue.Start());
  Finish(AttachmentSendOutcome::Sent);
  Finish(AttachmentSendOutcome::RateLimited); // p2 refused by the pace: not a failure
  EXPECT_EQ(fake_.released.count("p2.jpg"), 0u) << "kept for the next try";
  EXPECT_EQ(fake_.ticks_asked, (std::vector<int64_t>{PictureSendQueue::kRateLimitRetryMs}));
  EXPECT_TRUE(queue.Active()) << "the slot stays the queue's while it waits";
  EXPECT_TRUE(queue.Progress().paced);
  EXPECT_EQ(queue.Progress().number, 2u);
  fake_.now += PictureSendQueue::kRateLimitRetryMs - 1;
  queue.Tick(); // early: asks again for the rest
  EXPECT_EQ(Count("send p2.jpg"), 1u);
  EXPECT_EQ(fake_.ticks_asked.back(), 1);
  fake_.now += 1;
  queue.Tick();
  EXPECT_EQ(Count("send p2.jpg"), 2u) << "the same item again";
  queue.Tick(); // a spare tick does nothing
  EXPECT_EQ(Count("send "), 3u);
  Finish(AttachmentSendOutcome::Sent);
  Finish(AttachmentSendOutcome::Sent);
  EXPECT_EQ(fake_.report->exit, PictureSendExit::Finished);
  EXPECT_EQ(fake_.report->sent, 3);
  EXPECT_EQ(fake_.report->text, PictureTextFate::Sent);
  EXPECT_EQ(fake_.offered_while_busy, 0);
  ExpectEachReleasedOnce({"p1.jpg", "p2.jpg", "p3.jpg"});
}

TEST_F(PictureSendQueueTest, RateLimitedAgainAndAgainNeverGivesUpByItself) {
  PictureSendQueue& queue = MakeThree();
  ASSERT_TRUE(queue.Start());
  for (int i = 0; i < 20; ++i) {
    Finish(AttachmentSendOutcome::RateLimited);
    fake_.now += PictureSendQueue::kRateLimitRetryMs;
    queue.Tick();
  }
  EXPECT_TRUE(queue.Active());
  EXPECT_EQ(fake_.done_calls, 0);
  EXPECT_EQ(Count("send p1.jpg"), 21u);
}

// --- The slot between items ---

// An attachment from outside (camera, a file, a recording) is refused while AttachmentBusy() is true; the queue keeps
// the slot in every gap, so none can take it, and the queue never offers a second item while one is out.
TEST_F(PictureSendQueueTest, TheSlotIsHeldInEveryGapAndNoItemIsOfferedWhileOneIsOut) {
  PictureSendQueue& queue = MakeThree();
  EXPECT_FALSE(queue.Active()) << "choosing does not hold the slot";
  ASSERT_TRUE(queue.Start());
  EXPECT_TRUE(queue.Active());
  for (int i = 0; i < 3; ++i) {
    queue.OnItemFinished(fake_.in_flight + 100, AttachmentSendOutcome::Sent); // someone else's end: ignored
    EXPECT_TRUE(queue.InFlight());
    Finish(i == 1 ? AttachmentSendOutcome::RateLimited : AttachmentSendOutcome::Sent);
    if (i == 1) {
      EXPECT_TRUE(queue.Active()) << "in the pace wait";
      EXPECT_FALSE(queue.InFlight());
      fake_.now += PictureSendQueue::kRateLimitRetryMs;
      queue.Tick();
      Finish(AttachmentSendOutcome::Sent);
      continue;
    }
    if (i < 2) {
      EXPECT_TRUE(queue.Active()) << "between items";
    }
  }
  EXPECT_FALSE(queue.Active());
  EXPECT_EQ(fake_.offered_while_busy, 0);
  EXPECT_EQ(fake_.report->sent, 3) << "nothing dropped";
}

TEST_F(PictureSendQueueTest, ASecondReportOfTheSameItemIsIgnored) {
  PictureSendQueue& queue = MakeThree();
  ASSERT_TRUE(queue.Start());
  const uint64_t first = fake_.in_flight;
  Finish(AttachmentSendOutcome::Sent);
  queue.OnItemFinished(first, AttachmentSendOutcome::FailedNotStaged); // the double callback
  EXPECT_EQ(Count("send "), 2u) << "p2 was not skipped or started twice";
  EXPECT_EQ(fake_.released["p1.jpg"], 1);
  EXPECT_EQ(queue.Items()[0].state, PictureItemState::Sent);
}

TEST_F(PictureSendQueueTest, ARefusalAnsweredAtOnceMovesOnWithoutRecursion) {
  PictureSendQueue& queue = MakeThree();
  queue.SetText("look");
  fake_.answer_at_once = AttachmentSendOutcome::FailedNotStaged;
  ASSERT_TRUE(queue.Start());
  EXPECT_EQ(fake_.events, (std::vector<std::string>{"send p1.jpg", "release p1.jpg", "send p2.jpg", "release p2.jpg",
                                                    "send p3.jpg", "release p3.jpg", "text chat-a look", "done"}));
  EXPECT_EQ(fake_.report->failed_not_staged, 3);
}

TEST_F(PictureSendQueueTest, SendingWaitsForCopiesStillArrivingBeforeTheText) {
  PictureSendQueue& queue = Make();
  queue.SetArriving(true);
  (void)queue.Add("p1.jpg", PictureSendKind::Picture, 10);
  queue.SetText("look");
  ASSERT_TRUE(queue.Start());
  Finish(AttachmentSendOutcome::Sent);
  EXPECT_TRUE(queue.Active()) << "p2 is still being copied";
  EXPECT_EQ(Count("text "), 0u);
  (void)queue.Add("p2.jpg", PictureSendKind::Picture, 10);
  EXPECT_EQ(Count("send p2.jpg"), 1u);
  Finish(AttachmentSendOutcome::Sent);
  EXPECT_EQ(Count("text "), 0u);
  queue.SetArriving(false);
  EXPECT_EQ(Count("text chat-a look"), 1u);
  EXPECT_EQ(fake_.report->sent, 2);
}

// --- The plan's exit table ("文字去向"), one test per row ---

TEST_F(PictureSendQueueTest, ExitCancelledGivesUpTheRestAndPutsTheTextBack) {
  PictureSendQueue& queue = MakeThree();
  queue.SetText("look");
  ASSERT_TRUE(queue.Start());
  Finish(AttachmentSendOutcome::Sent);
  queue.Cancel(); // p2 is being sent: it cannot be taken back
  EXPECT_EQ(Count("text "), 0u);
  EXPECT_EQ(Count("put back chat-a look"), 1u);
  ASSERT_TRUE(fake_.report);
  EXPECT_EQ(fake_.report->exit, PictureSendExit::Cancelled);
  EXPECT_EQ(fake_.report->text, PictureTextFate::PutBack);
  EXPECT_EQ(fake_.report->cancelled, 1);
  EXPECT_EQ(fake_.report->in_flight, 1);
  EXPECT_FALSE(queue.Active());
  EXPECT_FALSE(queue.Settled()) << "p2's end is still taken";
  ExpectEachReleasedOnce({"p1.jpg", "p3.jpg"});
  Finish(AttachmentSendOutcome::Sent);
  EXPECT_TRUE(queue.Settled());
  EXPECT_EQ(fake_.done_calls, 1);
  EXPECT_EQ(Count("send "), 2u);
  ExpectEachReleasedOnce({"p1.jpg", "p2.jpg", "p3.jpg"});
}

TEST_F(PictureSendQueueTest, ExitCancelledDuringThePaceWaitEndsAtOnce) {
  PictureSendQueue& queue = MakeThree();
  ASSERT_TRUE(queue.Start());
  Finish(AttachmentSendOutcome::RateLimited);
  queue.Cancel();
  EXPECT_TRUE(queue.Settled());
  fake_.now += PictureSendQueue::kRateLimitRetryMs;
  queue.Tick();
  EXPECT_EQ(Count("send "), 1u);
  ExpectEachReleasedOnce({"p1.jpg", "p2.jpg", "p3.jpg"});
}

TEST_F(PictureSendQueueTest, ExitOfflineStopsAfterTheFailingItemAndPutsTheTextBack) {
  PictureSendQueue& queue = MakeThree();
  queue.SetText("look");
  ASSERT_TRUE(queue.Start());
  Finish(AttachmentSendOutcome::Sent);
  fake_.offline = true;
  Finish(AttachmentSendOutcome::FailedStaged); // p2 failed while offline
  EXPECT_EQ(Count("send "), 2u) << "p3 is not tried";
  ASSERT_TRUE(fake_.report);
  EXPECT_EQ(fake_.report->exit, PictureSendExit::Offline);
  EXPECT_EQ(fake_.report->text, PictureTextFate::PutBack);
  EXPECT_EQ(fake_.report->failed_staged, 1);
  EXPECT_EQ(fake_.report->cancelled, 1) << "the given-up count";
  EXPECT_EQ(Count("text "), 0u);
  EXPECT_EQ(Count("put back chat-a look"), 1u);
  EXPECT_TRUE(queue.Settled());
  ExpectEachReleasedOnce({"p1.jpg", "p2.jpg", "p3.jpg"});
}

TEST_F(PictureSendQueueTest, ExitOfflineAlsoWhenTheLastItemFailsAndBeforeAnyTry) {
  {
    PictureSendQueue& queue = Make();
    (void)queue.Add("p1.jpg", PictureSendKind::Picture, 10);
    queue.SetText("look");
    ASSERT_TRUE(queue.Start());
    fake_.offline = true;
    Finish(AttachmentSendOutcome::FailedNotStaged);
    EXPECT_EQ(fake_.report->exit, PictureSendExit::Offline);
    EXPECT_EQ(Count("text "), 0u) << "never sent while offline";
  }
  fake_ = Fake{};
  fake_.offline = true;
  PictureSendQueue& queue = MakeThree();
  queue.SetText("look");
  ASSERT_TRUE(queue.Start());
  EXPECT_EQ(Count("send "), 0u);
  EXPECT_EQ(fake_.report->exit, PictureSendExit::Offline);
  EXPECT_EQ(fake_.report->cancelled, 3);
  ExpectEachReleasedOnce({"p1.jpg", "p2.jpg", "p3.jpg"});
}

TEST_F(PictureSendQueueTest, ExitBackSendsNothingReleasesEverythingAndPutsTheTextBack) {
  PictureSendQueue& queue = MakeThree();
  ASSERT_TRUE(queue.SetPreview(queue.Items()[2].id, "pv3", "th3"));
  queue.SetText("look");
  queue.Back();
  EXPECT_EQ(Count("send "), 0u);
  EXPECT_EQ(fake_.report->exit, PictureSendExit::Back);
  EXPECT_EQ(fake_.report->text, PictureTextFate::PutBack);
  EXPECT_EQ(Count("put back chat-a look"), 1u);
  EXPECT_EQ(fake_.removed, (std::vector<std::string>{"pv3", "th3"}));
  ExpectEachReleasedOnce({"p1.jpg", "p2.jpg", "p3.jpg"});
  EXPECT_TRUE(queue.Settled());
  EXPECT_FALSE(queue.Add("late.jpg", PictureSendKind::Picture, 1).has_value()) << "a copy arriving after it";
  ExpectEachReleasedOnce({"late.jpg"});
  EXPECT_FALSE(queue.Start());
}

TEST_F(PictureSendQueueTest, ExitBackWithoutTextPutsNothingBack) {
  PictureSendQueue& queue = MakeThree();
  queue.Back();
  EXPECT_EQ(Count("put back"), 0u);
  EXPECT_EQ(fake_.report->text, PictureTextFate::None);
}

TEST_F(PictureSendQueueTest, ExitChatDeletedDropsTheTextAndReleasesTheRest) {
  PictureSendQueue& queue = MakeThree();
  queue.SetText("look");
  ASSERT_TRUE(queue.Start());
  queue.ChatDeleted(); // p1 in flight
  EXPECT_EQ(fake_.report->exit, PictureSendExit::ChatDeleted);
  EXPECT_EQ(fake_.report->text, PictureTextFate::Dropped);
  EXPECT_EQ(Count("put back"), 0u) << "never revived in a deleted chat's draft";
  EXPECT_EQ(Count("text "), 0u);
  ExpectEachReleasedOnce({"p2.jpg", "p3.jpg"});
  Finish(AttachmentSendOutcome::FailedNotStaged);
  ExpectEachReleasedOnce({"p1.jpg", "p2.jpg", "p3.jpg"});
  EXPECT_EQ(Count("send "), 1u);
}

TEST_F(PictureSendQueueTest, ExitLockedDropsTheTextAndReleasesTheRest) {
  for (const bool started : {false, true}) {
    SCOPED_TRACE(started ? "while sending" : "while choosing");
    fake_ = Fake{};
    PictureSendQueue& queue = MakeThree();
    queue.SetText("look");
    if (started) {
      ASSERT_TRUE(queue.Start());
      Finish(AttachmentSendOutcome::RateLimited); // in the pace wait
    }
    queue.Locked();
    EXPECT_EQ(fake_.report->exit, PictureSendExit::Locked);
    EXPECT_EQ(fake_.report->text, PictureTextFate::Dropped);
    EXPECT_EQ(Count("put back"), 0u);
    EXPECT_TRUE(queue.Settled());
    ExpectEachReleasedOnce({"p1.jpg", "p2.jpg", "p3.jpg"});
    queue.Tick();
    EXPECT_EQ(Count("send "), started ? 1u : 0u);
  }
}

TEST_F(PictureSendQueueTest, ExitSignedOutDropsTheTextAndReleasesTheRest) {
  PictureSendQueue& queue = MakeThree();
  queue.SetText("look");
  ASSERT_TRUE(queue.Start());
  Finish(AttachmentSendOutcome::Sent);
  queue.SignedOut();
  EXPECT_EQ(fake_.report->exit, PictureSendExit::SignedOut);
  EXPECT_EQ(fake_.report->text, PictureTextFate::Dropped);
  EXPECT_EQ(fake_.report->sent, 1);
  EXPECT_EQ(fake_.report->in_flight, 1);
  Finish(AttachmentSendOutcome::RateLimited); // after the end: not tried again, released
  EXPECT_EQ(Count("send "), 2u);
  ExpectEachReleasedOnce({"p1.jpg", "p2.jpg", "p3.jpg"});
}

TEST_F(PictureSendQueueTest, OnlyOneEndIsReportedWhateverComesAfter) {
  PictureSendQueue& queue = MakeThree();
  queue.SetText("look");
  queue.Back();
  queue.Locked();
  queue.SignedOut();
  queue.ChatDeleted();
  queue.Cancel();
  EXPECT_EQ(fake_.done_calls, 1);
  EXPECT_EQ(fake_.report->exit, PictureSendExit::Back);
  EXPECT_EQ(Count("put back"), 1u);
}

TEST(PictureSendTitleKeyTest, OneItemIsTitledAsAFileAndSeveralAreCounted) {
  EXPECT_STREQ(PictureSendItemsTitleKey(1), "picture_send.title_one_file");
  EXPECT_STREQ(PictureSendItemsTitleKey(2), "picture_send.title_items");
  EXPECT_STREQ(PictureSendItemsTitleKey(9), "picture_send.title_items");
}

} // namespace
} // namespace pbr
