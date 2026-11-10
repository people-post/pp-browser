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
#include <optional>
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
      result.account_id = account_; // read on the worker before the network
      if (renew_fails_) {
        result.error = Error("renew failed");
      } else if (renew_due_) {
        result.registration = RegistrationResult{.success = true, .relay_user_id = "relay:u1"};
      }
      return result;
    };
    ports.renew_persist = [this](const RegistrationResult& registration, const std::string& account) -> Roe<void> {
      EXPECT_FALSE(in_worker_) << "identity written on the UI thread only";
      if (account != stored_account_) {
        return Error("another account"); // what PersistRenewal does (registration_client_test)
      }
      persisted_.push_back("renew:" + registration.relay_user_id);
      return {};
    };
    ports.renew_hint = [this](const RenewResult& result, const bool auto_renew) {
      EXPECT_FALSE(in_worker_);
      hints_.push_back({result.registration.has_value(), result.error.has_value(), auto_renew});
    };
    ports.brief_key_needed = [this] { return stored_key_.empty(); };
    ports.account_id = [this] {
      EXPECT_FALSE(in_worker_);
      return account_;
    };
    ports.mint_brief_key_network = [this](const std::string& base_url) -> Roe<SecretText> {
      EXPECT_TRUE(in_worker_);
      ++mint_calls_;
      mint_base_urls_.push_back(base_url);
      if (mint_fails_) {
        return Error("mint failed");
      }
      return SecretText("brf_guest_new");
    };
    ports.brief_key_persist = [this](const SecretText& key, const std::string& account) -> Roe<void> {
      EXPECT_FALSE(in_worker_);
      if (account != stored_account_) {
        return Error("another account"); // what StoreBriefGuestKeyIfNone does (brief_llm_key_renewal_test)
      }
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
  std::string account_ = "account:a";        // what the identity says when a task reads it
  std::string stored_account_ = "account:a"; // what it says when the result is stored
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

TEST_F(MessagingReadyTasksTest, ATeardownFreesARunWhoseWorkWasDropped) {
  tasks_->SyncPushDevices();
  jobs_.clear(); // a teardown dropped it: its UI part never comes
  EXPECT_TRUE(tasks_->PushInFlight());
  ++session_;
  tasks_->OnMessagingTornDown();
  tasks_->SyncPushDevices();
  ASSERT_EQ(jobs_.size(), 1u);
  RunAll();
  EXPECT_EQ(push_calls_, 1);
  EXPECT_FALSE(tasks_->PushInFlight());
}

// A plain session change (the link import's binding, then WireMessagingBindings) while a binding is out: still one at
// a time; the running one ends (its result stale) and exactly one more runs after it.
TEST_F(MessagingReadyTasksTest, ASessionChangeKeepsOneRunAtATime) {
  tasks_->SyncPushDevices(); // on_link_imported
  ASSERT_EQ(jobs_.size(), 1u);
  RunWorkerPart(); // its worker is running
  ++session_;      // WireMessagingBindings
  tasks_->SyncPushDevices();
  EXPECT_EQ(jobs_.size(), 1u) << "no second binding beside the running one";
  EXPECT_EQ(push_calls_, 1);
  RunUiPart(); // the first ends: one more is due
  ASSERT_EQ(jobs_.size(), 1u);
  RunAll();
  EXPECT_EQ(push_calls_, 2);
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
    return RunSharedBriefKeyRenewal(*brief_run_, 10s, {}, [&]() -> std::string {
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
  const std::optional<std::string> got = renewal.get();
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, "reused:brf_guest_new");
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

// A turn stopped (Stop, profile reset, quit) while it waits for the mint gives up within one slice: the UI joining
// turns never waits for the mint.
TEST_F(MessagingReadyTasksTest, AStoppedRenewalGivesUpTheWaitWithinASlice) {
  tasks_->MintBriefKey("u");
  ASSERT_TRUE(brief_run_->InFlight());
  std::atomic<bool> cancelled{false};
  std::atomic<int> renews{0};
  auto renewal = std::async(std::launch::async, [&]() {
    return RunSharedBriefKeyRenewal(
        *brief_run_, kBriefKeyMintWait, [&]() { return cancelled.load(); },
        [&]() {
          ++renews;
          return 1;
        });
  });
  std::this_thread::sleep_for(50ms);
  const auto stopped_at = std::chrono::steady_clock::now();
  cancelled = true;
  ASSERT_EQ(renewal.wait_for(kBriefKeyWaitSlice + 500ms), std::future_status::ready) << "gave up within a slice";
  EXPECT_LT(std::chrono::steady_clock::now() - stopped_at, kBriefKeyWaitSlice + 500ms);
  EXPECT_FALSE(renewal.get().has_value()) << "nothing renewed";
  EXPECT_EQ(renews.load(), 0);
  EXPECT_TRUE(brief_run_->InFlight()) << "the mint's run is left as it was";
  RunAll();
  EXPECT_FALSE(brief_run_->InFlight());
}

// Hardening: a result is stored only into the account the task ran for (the profile changed meanwhile: refused).
TEST_F(MessagingReadyTasksTest, ResultsCarryTheAccountTheyRanFor) {
  tasks_->RenewRegistration();
  tasks_->MintBriefKey("u");
  stored_account_ = "account:b"; // same session and still ready, but the identity is another account now
  RunAll();
  EXPECT_TRUE(persisted_.empty()) << "nothing written into account b";
  ASSERT_EQ(hints_.size(), 1u);
  EXPECT_TRUE(hints_[0].failed);
  EXPECT_EQ(brief_ends_, (std::vector<BriefKeyMintOutcome>{BriefKeyMintOutcome::SaveFailed}));
  EXPECT_EQ(identity_changed_, 0);

  stored_account_ = "account:a";
  tasks_->RenewRegistration();
  RunAll();
  EXPECT_EQ(persisted_, (std::vector<std::string>{"renew:relay:u1"}));
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
