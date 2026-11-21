// Voice messages recorded while an earlier one is still being sent: the line they wait in, the order they go out in,
// a failure that never holds the line, a retry that joins the end of it, a cancel that leaves it, and what a lock, a
// sign-out or a deleted chat does to the ones that have not started.
#include "feature/presenters/VoiceSendQueue.h"

#include <gtest/gtest.h>

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
  rig.End("c", AttachmentSendOutcome::RateLimited);
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

} // namespace
} // namespace pbr
