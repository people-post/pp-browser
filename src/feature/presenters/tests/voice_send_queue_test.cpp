// Voice messages recorded while an earlier one is still being sent: the line they wait in, the order they go out in,
// a failure that never holds the line, a retry that joins the end of it, a cancel that leaves it, and what a lock, a
// sign-out or a deleted chat does to the ones that have not started.
#include "feature/presenters/VoiceSendQueue.h"

#include <gtest/gtest.h>

#include <algorithm>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace pbr {
namespace {

struct Fake {
  std::vector<std::string> started; // row ids, in the order they started
  std::vector<std::string> removed; // files deleted
  std::vector<std::string> reported;
  std::vector<AttachmentSendOutcome> outcomes;
  std::vector<VoiceSendTiming> timings;
  bool slot_free = true;
  int64_t now = 100;
  int started_while_running = 0;
  int running = 0;
  VoiceSendQueue* queue = nullptr;
  bool finish_at_once = false;
};

VoiceSendPorts PortsOf(Fake& fake) {
  VoiceSendPorts ports;
  ports.slot_free = [&fake]() { return fake.slot_free; };
  ports.start = [&fake](const VoiceSendJob& job) {
    if (fake.running++ > 0) {
      ++fake.started_while_running;
    }
    fake.started.push_back(job.row_id);
    if (fake.finish_at_once && fake.queue) {
      --fake.running;
      fake.queue->OnFinished(job.row_id, AttachmentSendOutcome::Sent);
    }
  };
  ports.remove_file = [&fake](const std::string& path) { fake.removed.push_back(path); };
  ports.now_ms = [&fake]() { return fake.now; };
  ports.report = [&fake](const VoiceSendJob& job, const AttachmentSendOutcome outcome, const VoiceSendTiming& timing) {
    fake.reported.push_back(job.row_id);
    fake.outcomes.push_back(outcome);
    fake.timings.push_back(timing);
  };
  return ports;
}

struct Rig {
  Fake fake;
  std::unique_ptr<VoiceSendQueue> queue;
  Rig() {
    queue = std::make_unique<VoiceSendQueue>(PortsOf(fake));
    fake.queue = queue.get();
  }
  void Add(const std::string& id, const std::string& thread = "chat-a", VoiceJobKind kind = VoiceJobKind::Fresh) {
    VoiceSendJob job;
    job.kind = kind;
    job.row_id = id;
    job.thread_id = thread;
    job.path = kind == VoiceJobKind::Fresh ? "/rec/" + id + ".ogg" : "";
    queue->Enqueue(std::move(job));
  }
  void End(const std::string& id, AttachmentSendOutcome outcome = AttachmentSendOutcome::Sent) {
    --fake.running;
    queue->OnFinished(id, outcome);
  }
};

TEST(VoiceSendQueue, TheFirstGoesAtOnceTheOthersWaitAndGoInOrder) {
  Rig rig;
  rig.Add("a");
  rig.Add("b");
  rig.Add("c");
  EXPECT_EQ(rig.fake.started, (std::vector<std::string>{"a"}));
  EXPECT_EQ(rig.queue->WaitingCount(), 2u);
  rig.End("a");
  EXPECT_EQ(rig.fake.started, (std::vector<std::string>{"a", "b"}));
  rig.End("b");
  EXPECT_EQ(rig.fake.started, (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_EQ(rig.fake.started_while_running, 0);
}

TEST(VoiceSendQueue, ARecordingMadeWhileAnotherUploadsJoinsTheLine) {
  Rig rig;
  rig.Add("a");
  rig.Add("b"); // recorded while a is being sent
  EXPECT_TRUE(rig.queue->IsWaiting("b"));
  EXPECT_TRUE(rig.queue->InFlight());
  EXPECT_EQ(rig.fake.started.size(), 1u);
}

TEST(VoiceSendQueue, AFailureDoesNotHoldTheLine) {
  Rig rig;
  rig.Add("a");
  rig.Add("b");
  rig.Add("c");
  rig.End("a", AttachmentSendOutcome::FailedStaged);
  EXPECT_EQ(rig.fake.started.back(), "b");
  rig.End("b", AttachmentSendOutcome::FailedNotStaged);
  EXPECT_EQ(rig.fake.started.back(), "c");
  rig.End("c", AttachmentSendOutcome::Cancelled); // (a pace refusal is not an end: ABurstPastTheSendPace...)
  EXPECT_EQ(rig.fake.outcomes.size(), 3u);
  EXPECT_FALSE(rig.queue->InFlight());
  EXPECT_EQ(rig.queue->WaitingCount(), 0u);
}

TEST(VoiceSendQueue, WaitsWhileAnotherAttachmentHoldsTheSlotAndGoesWhenPumped) {
  Rig rig;
  rig.fake.slot_free = false; // a picture is uploading
  rig.Add("a");
  rig.Add("b");
  EXPECT_TRUE(rig.fake.started.empty());
  rig.queue->Pump();
  EXPECT_TRUE(rig.fake.started.empty());
  rig.fake.slot_free = true; // the picture ended
  rig.queue->Pump();
  EXPECT_EQ(rig.fake.started, (std::vector<std::string>{"a"}));
  EXPECT_EQ(rig.queue->WaitingCount(), 1u);
}

TEST(VoiceSendQueue, RetryJoinsTheEndOfTheLine) {
  Rig rig;
  rig.Add("a");
  rig.Add("b");
  rig.Add("failed-earlier", "chat-a", VoiceJobKind::Retry);
  rig.End("a");
  rig.End("b");
  EXPECT_EQ(rig.fake.started, (std::vector<std::string>{"a", "b", "failed-earlier"}));
  EXPECT_TRUE(rig.fake.removed.empty()) << "a retry's staged copy is not a file the queue owns";
}

TEST(VoiceSendQueue, CancellingAWaitingJobTakesItOutAndDeletesItsRecording) {
  Rig rig;
  rig.Add("a");
  rig.Add("b");
  rig.Add("c");
  const auto cancelled = rig.queue->CancelWaiting("b");
  ASSERT_TRUE(cancelled.has_value());
  EXPECT_EQ(cancelled->row_id, "b");
  EXPECT_EQ(rig.fake.removed, (std::vector<std::string>{"/rec/b.ogg"}));
  rig.End("a");
  EXPECT_EQ(rig.fake.started, (std::vector<std::string>{"a", "c"}));
}

TEST(VoiceSendQueue, TheJobInFlightIsNotCancelledHere) {
  Rig rig;
  rig.Add("a");
  EXPECT_FALSE(rig.queue->CancelWaiting("a").has_value());
  EXPECT_FALSE(rig.queue->CancelWaiting("nobody").has_value());
  EXPECT_TRUE(rig.queue->InFlight());
}

TEST(VoiceSendQueue, CancellingAWaitingRetryKeepsNoFileToDelete) {
  Rig rig;
  rig.Add("a");
  rig.Add("r", "chat-a", VoiceJobKind::Retry);
  ASSERT_TRUE(rig.queue->CancelWaiting("r").has_value());
  EXPECT_TRUE(rig.fake.removed.empty());
}

TEST(VoiceSendQueue, ALockOrSignOutDropsTheWaitingOnesNotTheOneInFlight) {
  Rig rig;
  rig.Add("a");
  rig.Add("b");
  rig.Add("r", "chat-b", VoiceJobKind::Retry);
  const auto dropped = rig.queue->DropWaiting();
  ASSERT_EQ(dropped.size(), 2u);
  EXPECT_EQ(dropped[0].row_id, "b");
  EXPECT_EQ(dropped[1].row_id, "r");
  EXPECT_EQ(rig.fake.removed, (std::vector<std::string>{"/rec/b.ogg"}));
  EXPECT_TRUE(rig.queue->InFlight());
  rig.End("a");
  EXPECT_EQ(rig.fake.started.size(), 1u);
}

TEST(VoiceSendQueue, ADeletedChatDropsOnlyItsOwnWaitingJobs) {
  Rig rig;
  rig.Add("a", "chat-a");
  rig.Add("b", "chat-b");
  rig.Add("c", "chat-a");
  const auto dropped = rig.queue->DropWaiting(std::string("chat-b"));
  ASSERT_EQ(dropped.size(), 1u);
  EXPECT_EQ(dropped[0].row_id, "b");
  rig.End("a");
  EXPECT_EQ(rig.fake.started, (std::vector<std::string>{"a", "c"}));
}

TEST(VoiceSendQueue, AStaleOrSecondEndIsIgnored) {
  Rig rig;
  rig.Add("a");
  rig.Add("b");
  rig.queue->OnFinished("b", AttachmentSendOutcome::Sent); // b is waiting, not in flight
  EXPECT_TRUE(rig.queue->InFlight());
  EXPECT_EQ(rig.fake.started.size(), 1u);
  rig.End("a");
  rig.queue->OnFinished("a", AttachmentSendOutcome::Sent); // a second report of a
  EXPECT_EQ(rig.fake.reported, (std::vector<std::string>{"a"}));
  EXPECT_EQ(rig.fake.started.size(), 2u);
}

TEST(VoiceSendQueue, AJobThatEndsInsideStartDoesNotRecurseAndTheNextStillGoes) {
  Rig rig;
  rig.fake.finish_at_once = true;
  rig.Add("a");
  rig.Add("b");
  rig.Add("c");
  EXPECT_EQ(rig.fake.started, (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_EQ(rig.fake.reported.size(), 3u);
  EXPECT_FALSE(rig.queue->InFlight());
}

TEST(VoiceSendQueue, TheTimingSaysHowLongAJobWaitedAndHowLongItTook) {
  Rig rig;
  rig.fake.now = 1000;
  rig.Add("a");
  rig.fake.now = 1500;
  rig.Add("b"); // waits behind a
  rig.fake.now = 4000;
  rig.End("a"); // a: 0 ms wait, 3000 ms; b starts now
  rig.fake.now = 9000;
  rig.End("b"); // b: 2500 ms wait, 5000 ms
  ASSERT_EQ(rig.fake.timings.size(), 2u);
  EXPECT_EQ(rig.fake.timings[0].wait_ms, 0);
  EXPECT_EQ(rig.fake.timings[0].send_ms, 3000);
  EXPECT_EQ(rig.fake.timings[1].wait_ms, 2500);
  EXPECT_EQ(rig.fake.timings[1].send_ms, 5000);
}

// At most kVoiceSendMaxPerChat (5) voice messages of one chat waiting or being sent: the one in flight counts, a failed
// one does not (it has left the line), and each chat counts its own.
TEST(VoiceSendQueue, AChatIsFullAtFiveWaitingOrSending) {
  Rig rig;
  int changes = 0;
  // A fresh queue whose `changed` port counts (the chat screen refreshes the bar on it).
  auto ports = PortsOf(rig.fake);
  ports.changed = [&changes]() { ++changes; };
  rig.queue = std::make_unique<VoiceSendQueue>(std::move(ports));
  rig.fake.queue = rig.queue.get();
  for (int i = 1; i <= 4; ++i) {
    rig.Add("a" + std::to_string(i));
    EXPECT_FALSE(rig.queue->ChatFull("chat-a")) << i;
  }
  rig.Add("a5");
  EXPECT_EQ(rig.queue->CountForThread("chat-a"), 5u);
  EXPECT_TRUE(rig.queue->ChatFull("chat-a"));
  EXPECT_EQ(changes, 5);
}

TEST(VoiceSendQueue, AFinishedOneFreesTheChat) {
  Rig rig;
  for (int i = 1; i <= 5; ++i) {
    rig.Add("a" + std::to_string(i));
  }
  ASSERT_TRUE(rig.queue->ChatFull("chat-a"));
  rig.End("a1");
  EXPECT_FALSE(rig.queue->ChatFull("chat-a"));
  EXPECT_EQ(rig.queue->CountForThread("chat-a"), 4u) << "a2 in flight and three waiting";
}

TEST(VoiceSendQueue, AFailedOneNoLongerCounts) {
  Rig rig;
  for (int i = 1; i <= 5; ++i) {
    rig.Add("a" + std::to_string(i));
  }
  rig.End("a1", AttachmentSendOutcome::FailedStaged);
  EXPECT_FALSE(rig.queue->ChatFull("chat-a"));
  EXPECT_EQ(rig.queue->CountForThread("chat-a"), 4u);
}

TEST(VoiceSendQueue, ACancelledOneFreesTheChat) {
  Rig rig;
  for (int i = 1; i <= 5; ++i) {
    rig.Add("a" + std::to_string(i));
  }
  ASSERT_TRUE(rig.queue->CancelWaiting("a3"));
  EXPECT_FALSE(rig.queue->ChatFull("chat-a"));
  rig.Add("a6");
  EXPECT_TRUE(rig.queue->ChatFull("chat-a"));
  rig.End("a1", AttachmentSendOutcome::Cancelled); // the one in flight cancelled by its own send
  EXPECT_FALSE(rig.queue->ChatFull("chat-a"));
}

TEST(VoiceSendQueue, EachChatCountsItsOwnAndTheLineStaysInOrder) {
  Rig rig;
  for (int i = 1; i <= 5; ++i) {
    rig.Add("a" + std::to_string(i), "chat-a");
  }
  rig.Add("b1", "chat-b");
  EXPECT_TRUE(rig.queue->ChatFull("chat-a"));
  EXPECT_FALSE(rig.queue->ChatFull("chat-b"));
  EXPECT_EQ(rig.queue->CountForThread("chat-b"), 1u);
  rig.Add("a6", "chat-a"); // not refused by the queue: only the bar is
  for (const std::string id : {"a1", "a2", "a3", "a4", "a5", "b1"}) {
    rig.End(id);
  }
  EXPECT_EQ(rig.fake.started, (std::vector<std::string>{"a1", "a2", "a3", "a4", "a5", "b1", "a6"}))
      << "first in, first out across chats";
}

TEST(VoiceSendQueue, DroppingAChatsWaitingOnesFreesIt) {
  Rig rig;
  for (int i = 1; i <= 5; ++i) {
    rig.Add("a" + std::to_string(i));
  }
  rig.queue->DropWaiting(std::string("chat-a"));
  EXPECT_EQ(rig.queue->CountForThread("chat-a"), 1u) << "only the one in flight";
}

// The recorder must not wipe a recording still in the line: the waiting ones and the one in flight, never a Retry's
// (it has no file of its own) and never one that has ended.
TEST(VoiceSendQueue, FilesInUseAreTheFreshRecordingsWaitingOrInFlight) {
  Rig rig;
  rig.Add("a");
  rig.Add("b");
  rig.Add("r", "chat-a", VoiceJobKind::Retry);
  EXPECT_EQ(rig.queue->FilesInUse(), (std::vector<std::string>{"/rec/a.ogg", "/rec/b.ogg"}));
  rig.End("a");
  EXPECT_EQ(rig.queue->FilesInUse(), (std::vector<std::string>{"/rec/b.ogg"}));
}

// A failed prepare offers a Retry only while the recording still exists and has bytes.
TEST(VoiceRecordingRetryable, OnlyAFileWithBytesIsOfferedARetry) {
  const auto dir = std::filesystem::temp_directory_path() / "pp-voice-retryable-test";
  std::filesystem::create_directories(dir);
  const std::string kept = (dir / "kept.ogg").string();
  const std::string empty = (dir / "empty.ogg").string();
  const std::string gone = (dir / "gone.ogg").string();
  std::filesystem::remove(gone);
  {
    std::ofstream(kept, std::ios::binary) << "ogg";
  }
  {
    std::ofstream(empty, std::ios::binary);
  }
  EXPECT_TRUE(VoiceRecordingRetryable(kept));
  EXPECT_TRUE(std::filesystem::exists(kept)) << "a retryable recording is left alone";
  EXPECT_FALSE(VoiceRecordingRetryable(empty));
  EXPECT_FALSE(std::filesystem::exists(empty)) << "an empty leftover is deleted, nothing lingers";
  EXPECT_FALSE(VoiceRecordingRetryable(gone));
  EXPECT_FALSE(VoiceRecordingRetryable(""));
  std::filesystem::remove_all(dir);
}

// A burst past this device's send pace (send_max_per_minute, 10 by default): the refused ones are not failures. Each
// waits at the head of the line and goes again; all twelve go, in the order recorded, and no recording is deleted.
TEST(VoiceSendQueue, ABurstPastTheSendPaceAllGoInOrder) {
  Fake fake;
  std::vector<int64_t> sent_at;
  std::vector<int64_t> ticks;
  std::unique_ptr<VoiceSendQueue> queue;
  auto ports = PortsOf(fake);
  ports.start = [&](const VoiceSendJob& job) {
    // The fake limiter: at most 10 sends in any 60 s.
    const auto recent =
        std::count_if(sent_at.begin(), sent_at.end(), [&](const int64_t at) { return fake.now - at < 60000; });
    if (recent >= 10) {
      queue->OnFinished(job.row_id, AttachmentSendOutcome::RateLimited);
      return;
    }
    sent_at.push_back(fake.now);
    fake.started.push_back(job.row_id);
    queue->OnFinished(job.row_id, AttachmentSendOutcome::Sent);
  };
  ports.schedule_tick = [&ticks](const int64_t delay_ms) { ticks.push_back(delay_ms); };
  queue = std::make_unique<VoiceSendQueue>(std::move(ports));
  std::vector<std::string> order;
  for (int i = 1; i <= 12; ++i) {
    order.push_back("v" + std::to_string(i));
    VoiceSendJob job;
    job.row_id = order.back();
    job.thread_id = "chat-a";
    job.path = "/rec/" + job.row_id + ".ogg";
    queue->Enqueue(std::move(job));
    fake.now += 100; // recorded one after another
  }
  EXPECT_EQ(fake.started.size(), 10u);
  EXPECT_EQ(queue->WaitingCount(), 2u) << "the refused one waits first in line, the last behind it";
  EXPECT_EQ(queue->CountForThread("chat-a"), 2u) << "still counted toward the cap";
  ASSERT_FALSE(ticks.empty());
  for (int guard = 0; guard < 20 && queue->WaitingCount() > 0; ++guard) {
    fake.now += ticks.back();
    queue->Tick();
  }
  EXPECT_EQ(fake.started, order) << "all twelve, in the order recorded";
  EXPECT_TRUE(fake.removed.empty()) << "no recording deleted";
  EXPECT_EQ(fake.reported.size(), 12u) << "a pace refusal is not an end";
  for (const AttachmentSendOutcome outcome : fake.outcomes) {
    EXPECT_EQ(outcome, AttachmentSendOutcome::Sent);
  }
}

TEST(VoiceSendQueue, NothingStartsBeforeThePaceWaitIsOver) {
  Rig rig;
  std::vector<int64_t> ticks;
  auto ports = PortsOf(rig.fake);
  ports.schedule_tick = [&ticks](const int64_t delay_ms) { ticks.push_back(delay_ms); };
  rig.queue = std::make_unique<VoiceSendQueue>(std::move(ports));
  rig.fake.queue = rig.queue.get();
  rig.Add("a");
  rig.Add("b");
  rig.End("a", AttachmentSendOutcome::RateLimited);
  ASSERT_EQ(ticks, (std::vector<int64_t>{VoiceSendQueue::kRetryAfterPaceMs}));
  rig.queue->Pump();
  rig.queue->Tick(); // too early: waits the rest
  EXPECT_EQ(rig.fake.started, (std::vector<std::string>{"a"}));
  EXPECT_EQ(ticks.size(), 2u);
  rig.fake.now += VoiceSendQueue::kRetryAfterPaceMs;
  rig.queue->Tick();
  EXPECT_EQ(rig.fake.started, (std::vector<std::string>{"a", "a"})) << "the same job first, then b";
  EXPECT_TRUE(rig.queue->CancelWaiting("b"));
}

// A Retry job's completion reports exactly once, also when the resume drops its callback without calling it.
TEST(VoiceSendQueue, ARetrysCompletionReportsOnceEvenWhenDropped) {
  std::vector<AttachmentSendOutcome> got;
  {
    auto done = VoiceRetryCompletion([&got](const AttachmentSendOutcome outcome) { got.push_back(outcome); });
  } // dropped uncalled
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], AttachmentSendOutcome::FailedStaged);
  got.clear();
  {
    auto done = VoiceRetryCompletion([&got](const AttachmentSendOutcome outcome) { got.push_back(outcome); });
    auto copy = done;
    done(true);
    copy(false);
  }
  EXPECT_EQ(got, (std::vector<AttachmentSendOutcome>{AttachmentSendOutcome::Sent}));
}

} // namespace
} // namespace pbr
