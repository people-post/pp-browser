// The messaging-ready network work off the UI thread (DEV/PLAN-unlock-gate-early-dismiss.md A): MessagingReadyTasks
// driven through its ports, no ChatController. A fake runner queues each task's worker part and UI part so the test
// decides when each runs. What is checked: the network half runs in the worker part and every persist / hint only in
// the UI part; a result of a stale session writes nothing; every way out frees the run; a trigger while one runs gives
// exactly one more run; the Brief key run shared with the 401 renewal never mints twice.
#include "gui/chat/MessagingReadyTasks.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pbr {
namespace {

using namespace std::chrono_literals;

class MessagingReadyTasksTest : public ::testing::Test {
protected:
  struct Job {
    std::function<void()> work;
    std::function<void()> on_ui;
  };

  MessagingReadyTasksTest() {
    MessagingReadyTasks::Ports ports;
    ports.run_worker_then_ui = [this](std::function<void()> work, std::function<void()> on_ui) {
      jobs_.push_back({std::move(work), std::move(on_ui)});
    };
    ports.session = [this] { return session_; };
    ports.still_ready = [this](const uint64_t started) { return started == session_ && ready_; };
    ports.register_push = [this] {
      EXPECT_TRUE(in_worker_) << "push binding off the UI part";
      ++push_calls_;
      if (push_throws_) {
        throw std::runtime_error("push failed");
      }
    };
    ports.auto_renew = [this] {
      EXPECT_FALSE(in_worker_) << "the preference is read on the UI thread";
      return auto_renew_pref_;
    };
    ports.renew_network = [this](const bool auto_renew) {
      EXPECT_TRUE(in_worker_);
      ++renew_calls_;
      renew_seen_auto_renew_.push_back(auto_renew);
      if (renew_throws_) {
        throw std::runtime_error("renew threw");
      }
      RenewResult result;
      if (renew_fails_) {
        result.error = Error("renew failed");
      } else if (renew_due_) {
        result.registration = RegistrationResult{.success = true, .relay_user_id = "relay:u1"};
      }
      return result;
    };
    ports.renew_persist = [this](const RegistrationResult& registration) -> Roe<void> {
      EXPECT_FALSE(in_worker_) << "identity written on the UI thread only";
      persisted_.push_back("renew:" + registration.relay_user_id);
      return {};
    };
    ports.renew_hint = [this](const RenewResult& result, const bool auto_renew) {
      EXPECT_FALSE(in_worker_);
      hints_.push_back({result.registration.has_value(), result.error.has_value(), auto_renew});
    };
    ports.brief_key_needed = [this] { return stored_key_.empty(); };
    ports.mint_brief_key_network = [this](const std::string& base_url) -> Roe<SecretText> {
      EXPECT_TRUE(in_worker_);
      ++mint_calls_;
      mint_base_urls_.push_back(base_url);
      if (mint_fails_) {
        return Error("mint failed");
      }
      return SecretText("brf_guest_new");
    };
    ports.brief_key_persist = [this](const SecretText& key) -> Roe<void> {
      EXPECT_FALSE(in_worker_);
      persisted_.push_back("brief:" + key.Reveal());
      stored_key_ = key.Reveal();
      return {};
    };
    ports.brief_key_done = [this](const BriefKeyMintEnd& end) {
      EXPECT_FALSE(brief_run_->InFlight()) << "the end is told once the run is free (the banner reads it)";
      brief_ends_.push_back(end.outcome);
    };
    ports.after_identity_changed = [this] { ++identity_changed_; };
    tasks_ = std::make_unique<MessagingReadyTasks>(std::move(ports), brief_run_);
  }

  /** The worker part of the oldest queued job; its UI part waits for RunUiPart. */
  void RunWorkerPart() {
    ASSERT_FALSE(jobs_.empty());
    in_worker_ = true;
    jobs_.front().work();
    in_worker_ = false;
  }
  void RunUiPart() {
    ASSERT_FALSE(jobs_.empty());
    Job job = std::move(jobs_.front());
    jobs_.pop_front();
    job.on_ui();
  }
  void RunNext() {
    RunWorkerPart();
    RunUiPart();
  }
  void RunAll() {
    while (!jobs_.empty()) {
      RunNext();
    }
  }

  struct Hint {
    bool renewed;
    bool failed;
    bool auto_renew;
  };

  std::deque<Job> jobs_;
  bool in_worker_ = false;
  uint64_t session_ = 1;
  bool ready_ = true;
  int push_calls_ = 0;
  bool push_throws_ = false;
  bool auto_renew_pref_ = true;
  int renew_calls_ = 0;
  std::vector<bool> renew_seen_auto_renew_;
  bool renew_due_ = true;
  bool renew_fails_ = false;
  bool renew_throws_ = false;
  int mint_calls_ = 0;
  std::vector<std::string> mint_base_urls_;
  bool mint_fails_ = false;
  std::string stored_key_;
  std::vector<std::string> persisted_;
  std::vector<Hint> hints_;
  std::vector<BriefKeyMintOutcome> brief_ends_;
  int identity_changed_ = 0;
  std::shared_ptr<CoalescedRun> brief_run_ = std::make_shared<CoalescedRun>();
  std::unique_ptr<MessagingReadyTasks> tasks_;
};

TEST_F(MessagingReadyTasksTest, NetworkRunsInTheWorkerPartAndNothingIsStoredBeforeTheUiPart) {
  tasks_->SyncPushDevices();
  tasks_->RenewRegistration();
  tasks_->MintBriefKey("https://llm.example/api/llm/v1");
  ASSERT_EQ(jobs_.size(), 3u);
  EXPECT_EQ(push_calls_ + renew_calls_ + mint_calls_, 0) << "nothing runs on the calling (UI) thread";

  for (int i = 0; i < 3; ++i) {
    RunWorkerPart();
    jobs_.push_back(std::move(jobs_.front())); // keep its UI part for later
    jobs_.pop_front();
  }
  EXPECT_EQ(push_calls_, 1);
  EXPECT_EQ(renew_calls_, 1);
  EXPECT_EQ(mint_calls_, 1);
  EXPECT_TRUE(persisted_.empty()) << "no persist before the UI part";
  EXPECT_TRUE(hints_.empty());
  EXPECT_EQ(identity_changed_, 0);

  while (!jobs_.empty()) {
    RunUiPart();
  }
  EXPECT_EQ(persisted_, (std::vector<std::string>{"renew:relay:u1", "brief:brf_guest_new"}));
  ASSERT_EQ(hints_.size(), 1u);
  EXPECT_TRUE(hints_[0].renewed);
  EXPECT_EQ(brief_ends_, (std::vector<BriefKeyMintOutcome>{BriefKeyMintOutcome::Stored}));
  EXPECT_EQ(identity_changed_, 2) << "after the renewal and after the key";
  EXPECT_EQ(mint_base_urls_, (std::vector<std::string>{"https://llm.example/api/llm/v1"}));
  EXPECT_FALSE(tasks_->PushInFlight());
  EXPECT_FALSE(tasks_->RenewInFlight());
  EXPECT_FALSE(brief_run_->InFlight());
}

TEST_F(MessagingReadyTasksTest, AResultOfAnOlderSessionWritesNothingAndFreesItsRun) {
  tasks_->RenewRegistration();
  tasks_->MintBriefKey("u");
  RunWorkerPart();
  jobs_.push_back(std::move(jobs_.front()));
  jobs_.pop_front();
  RunWorkerPart();
  ++session_; // locked and unlocked again (WireMessagingBindings once more) while both were out
  RunUiPart();
  RunUiPart();
  EXPECT_TRUE(persisted_.empty()) << "a stale result is dropped";
  EXPECT_TRUE(hints_.empty());
  EXPECT_EQ(identity_changed_, 0);
  EXPECT_EQ(brief_ends_, (std::vector<BriefKeyMintOutcome>{BriefKeyMintOutcome::Discarded}));
  EXPECT_FALSE(tasks_->RenewInFlight());
  EXPECT_FALSE(brief_run_->InFlight());

  tasks_->RenewRegistration();
  tasks_->MintBriefKey("u");
  RunAll();
  EXPECT_EQ(renew_calls_, 2) << "the run is free again";
  EXPECT_EQ(mint_calls_, 2);
  EXPECT_EQ(persisted_, (std::vector<std::string>{"renew:relay:u1", "brief:brf_guest_new"}));
}

TEST_F(MessagingReadyTasksTest, MessagingNoLongerReadyWritesNothing) {
  tasks_->RenewRegistration();
  RunWorkerPart();
  ready_ = false;
  RunUiPart();
  EXPECT_TRUE(persisted_.empty());
  EXPECT_FALSE(tasks_->RenewInFlight());
}

TEST_F(MessagingReadyTasksTest, ThrowingOrFailingWorkFreesTheRun) {
  push_throws_ = true;
  renew_throws_ = true;
  mint_fails_ = true;
  tasks_->SyncPushDevices();
  tasks_->RenewRegistration();
  tasks_->MintBriefKey("u");
  RunAll();
  EXPECT_FALSE(tasks_->PushInFlight());
  EXPECT_FALSE(tasks_->RenewInFlight());
  EXPECT_FALSE(brief_run_->InFlight());
  ASSERT_EQ(hints_.size(), 1u);
  EXPECT_TRUE(hints_[0].failed) << "a renewal that threw is logged as failed";
  EXPECT_TRUE(persisted_.empty());
  EXPECT_EQ(brief_ends_, (std::vector<BriefKeyMintOutcome>{BriefKeyMintOutcome::MintFailed}));
  EXPECT_EQ(identity_changed_, 0);

  push_throws_ = false;
  renew_throws_ = false;
  renew_fails_ = true;
  tasks_->SyncPushDevices();
  tasks_->RenewRegistration();
  RunAll();
  EXPECT_EQ(push_calls_, 2);
  EXPECT_EQ(renew_calls_, 2);
  ASSERT_EQ(hints_.size(), 2u);
  EXPECT_TRUE(hints_[1].failed);
  EXPECT_FALSE(tasks_->RenewInFlight());
}

TEST_F(MessagingReadyTasksTest, ATriggerWhileOneRunsGivesExactlyOneMoreRun) {
  tasks_->SyncPushDevices();
  tasks_->SyncPushDevices(); // a token change while the first binding is out
  EXPECT_EQ(jobs_.size(), 1u) << "one at a time";
  RunAll();
  EXPECT_EQ(push_calls_, 2);

  push_calls_ = 0;
  tasks_->SyncPushDevices();
  tasks_->SyncPushDevices();
  tasks_->SyncPushDevices();
  EXPECT_EQ(jobs_.size(), 1u);
  RunAll();
  EXPECT_EQ(push_calls_, 2) << "three triggers: the running one and one more";
  EXPECT_FALSE(tasks_->PushInFlight());
}

TEST_F(MessagingReadyTasksTest, ANewSessionFreesARunWhoseWorkWasDropped) {
  tasks_->SyncPushDevices();
  jobs_.clear(); // a teardown dropped it: its UI part never comes
  EXPECT_TRUE(tasks_->PushInFlight());
  ++session_;
  tasks_->OnNewSession();
  tasks_->SyncPushDevices();
  ASSERT_EQ(jobs_.size(), 1u);
  RunAll();
  EXPECT_EQ(push_calls_, 1);
  EXPECT_FALSE(tasks_->PushInFlight());
}

TEST_F(MessagingReadyTasksTest, RenewHintGetsThePreferenceReadOnTheUiThread) {
  renew_due_ = false; // not due: the hint branch decides about "expires soon"
  auto_renew_pref_ = false;
  tasks_->RenewRegistration();
  auto_renew_pref_ = true; // changed after the dispatch: the task keeps what it read
  RunAll();
  ASSERT_EQ(renew_seen_auto_renew_.size(), 1u);
  EXPECT_FALSE(renew_seen_auto_renew_[0]);
  ASSERT_EQ(hints_.size(), 1u);
  EXPECT_FALSE(hints_[0].auto_renew);
  EXPECT_FALSE(hints_[0].renewed);
  EXPECT_TRUE(persisted_.empty()) << "nothing due, nothing stored";
  EXPECT_EQ(identity_changed_, 0);
}

TEST_F(MessagingReadyTasksTest, BriefKeyMintSuccessTellsTheIdentityChangedOnce) {
  tasks_->MintBriefKey("u");
  RunAll();
  EXPECT_EQ(identity_changed_, 1);
  EXPECT_EQ(stored_key_, "brf_guest_new");
  tasks_->MintBriefKey("u");
  EXPECT_TRUE(jobs_.empty()) << "a key is stored: no second mint";
}

// The 401 renewal (B183) on a turn's thread while the unlock's mint is out: it waits for that mint's key instead of
// minting a second one.
TEST_F(MessagingReadyTasksTest, The401RenewalWaitsForTheMintInFlightAndMintsNothing) {
  tasks_->MintBriefKey("u");
  ASSERT_TRUE(brief_run_->InFlight());

  std::atomic<int> renew_mints{0};
  std::atomic<bool> renewal_started{false};
  auto renewal = std::async(std::launch::async, [&]() {
    renewal_started = true;
    return RunSharedBriefKeyRenewal(*brief_run_, 10s, [&]() -> std::string {
      // RenewBriefLlmKey: the stored key changed (the mint's) -> reused; else it would mint.
      if (!stored_key_.empty()) {
        return "reused:" + stored_key_;
      }
      ++renew_mints;
      return std::string("minted");
    });
  });
  while (!renewal_started) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(renewal.wait_for(0ms), std::future_status::timeout) << "it waits while the mint is out";

  RunAll(); // the mint ends: key stored on the "UI thread", the run freed
  EXPECT_EQ(renewal.get(), "reused:brf_guest_new");
  EXPECT_EQ(renew_mints.load(), 0);
  EXPECT_EQ(mint_calls_, 1) << "one mint in all";
  EXPECT_EQ(identity_changed_, 1);
  EXPECT_TRUE(jobs_.empty()) << "the renewal waiting asked for no further mint";
  EXPECT_FALSE(brief_run_->InFlight());
}

TEST_F(MessagingReadyTasksTest, AMintAskedForWhileTheRenewalHeldTheRunIsToldOnce) {
  auto ticket = brief_run_->TryStart(/*queue_again=*/false); // the 401 renewal holds the run
  ASSERT_TRUE(ticket);
  tasks_->MintBriefKey("u");
  EXPECT_TRUE(jobs_.empty()) << "no mint beside the renewal";
  EXPECT_TRUE(brief_run_->Finish(*ticket)) << "the renewal is told a mint was asked for";
  tasks_->RetryBriefKeyMint(); // what Application does then (posted to the UI)
  ASSERT_EQ(jobs_.size(), 1u) << "no key yet: the mint runs now";
  RunAll();
  EXPECT_EQ(mint_calls_, 1);
}

TEST(CoalescedRunTest, AStaleTicketChangesNothing) {
  CoalescedRun run;
  auto first = run.TryStart();
  ASSERT_TRUE(first);
  run.Reset();
  auto second = run.TryStart();
  ASSERT_TRUE(second);
  EXPECT_FALSE(run.Finish(*first));
  EXPECT_TRUE(run.InFlight()) << "the old run's end does not free the new one";
  EXPECT_FALSE(run.Finish(*second));
  EXPECT_FALSE(run.InFlight());
}

} // namespace
} // namespace pbr
