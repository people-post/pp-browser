#include "feature/conversations/MyRelaysWorkflow.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

// Changing my relays (multi-relay R3, MR006; SERVICE_ENDPOINTS § My relays): each change is kept, the services
// follow it, our registration goes where it is new and leaves where we left, and the card with the new list is
// announced. Only the main device changes the list; a linked device follows the card's.

constexpr const char kA[] = "https://a.example/api/relay";
constexpr const char kB[] = "https://b.example/api/relay";
constexpr const char kC[] = "https://c.example/api/relay";

using Steps = std::vector<std::string>;
using Origins = std::vector<std::string>;

class MyRelaysWorkflowTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ =
        std::filesystem::temp_directory_path() / ("pp_my_relays_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    store_ = std::make_unique<OurRelaysStore>(dir_.string());
    store_->LoadOrSeed(kA, {kB});
    workflow_ = std::make_unique<MyRelaysWorkflow>(*store_, Ports());
  }
  void TearDown() override {
    workflow_.reset();
    store_.reset();
    std::filesystem::remove_all(dir_);
  }

  MyRelaysWorkflow::Ports Ports() {
    MyRelaysWorkflow::Ports ports;
    ports.probe = [this](const std::string& base_url) -> Roe<void> {
      steps_.push_back("probe " + base_url);
      if (!reachable_) {
        return Error("connect failed");
      }
      return {};
    };
    ports.main_device = [this]() { return main_device_; };
    ports.apply = [this]() {
      steps_.push_back("apply " + Join(store_->List().Origins()));
      if (on_apply_) {
        on_apply_();
      }
      return apply_ok_;
    };
    ports.leave = [this](const std::string& origin) -> Roe<void> {
      steps_.push_back("leave " + origin);
      if (!leave_ok_) {
        return Error("relay down");
      }
      return {};
    };
    ports.register_preferred = [this]() -> Roe<void> {
      steps_.push_back("register_preferred " + store_->List().Preferred());
      if (!preferred_ok_) {
        return Error("HTTP 503");
      }
      registered_at_ = store_->List().Preferred();
      return {};
    };
    ports.preferred_registration = [this]() -> std::optional<RelayRegistrationRecord> {
      if (registered_at_ != store_->List().Preferred()) {
        return std::nullopt;
      }
      return RelayRegistrationRecord{.state = RelayRegistrationRecord::State::Registered, .expires_at = "2099"};
    };
    ports.register_others = [this]() { steps_.push_back("register_others"); };
    ports.announce = [this]() { steps_.push_back("announce"); };
    ports.changed = [this]() { ++changed_; };
    return ports;
  }
  static std::string Join(const Origins& origins) {
    std::string out;
    for (const std::string& origin : origins) {
      out += (out.empty() ? "" : ",") + origin.substr(8, 1); // the host's letter
    }
    return out;
  }
  static std::string ErrorOf(const Roe<void>& result) { return result ? std::string("ok") : result.error().message; }

  std::filesystem::path dir_;
  std::unique_ptr<OurRelaysStore> store_;
  std::unique_ptr<MyRelaysWorkflow> workflow_;
  Steps steps_;
  bool reachable_ = true;
  bool main_device_ = true;
  bool leave_ok_ = true;
  bool preferred_ok_ = true;
  bool apply_ok_ = true;
  std::function<void()> on_apply_;
  std::string registered_at_ = kA;
  int changed_ = 0;
};

TEST_F(MyRelaysWorkflowTest, ARelayAddedAnswersIsKeptLastAndRegisteredOnAndTheCardGoes) {
  ASSERT_EQ(ErrorOf(workflow_->Add("https://C.example", false)), "ok");
  EXPECT_EQ(steps_, Steps({"probe " + std::string(kC), "apply a,b,c", "register_others", "announce"}));
  EXPECT_EQ(store_->List().Origins(), Origins({kA, kB, kC}));
  EXPECT_EQ(changed_, 1);
  OurRelaysStore reread(dir_.string());
  EXPECT_EQ(reread.LoadOrSeed(kA, {}).Origins(), Origins({kA, kB, kC})) << "kept in the profile";
}

TEST_F(MyRelaysWorkflowTest, ARelayThatDoesNotAnswerIsNotAdded) {
  reachable_ = false;
  EXPECT_EQ(ErrorOf(workflow_->Add("https://c.example", false)), MyRelaysWorkflow::kUnreachable);
  EXPECT_EQ(steps_, Steps({"probe " + std::string(kC)}));
  EXPECT_EQ(store_->List().Origins(), Origins({kA, kB}));
}

TEST_F(MyRelaysWorkflowTest, WhatIsNotAnHttpsOriginIsRefusedBeforeAnythingIsAsked) {
  EXPECT_EQ(ErrorOf(workflow_->Add("http://c.example", false)), "relay_origin_not_https");
  EXPECT_EQ(ErrorOf(workflow_->Add("https://c.example/inbox", false)), "relay_origin_has_path");
  EXPECT_EQ(ErrorOf(workflow_->Add("https://c.example?x", false)), "relay_origin_has_query");
  EXPECT_EQ(ErrorOf(workflow_->Add("https://b.example", false)), "relay_already_listed");
  EXPECT_TRUE(steps_.empty());
  EXPECT_EQ(ErrorOf(workflow_->Add("http://c.example", true)), "ok") << "a developer build's plain relay";
}

TEST_F(MyRelaysWorkflowTest, ARelayRemovedIsLeftFirstThenTheServicesFollow) {
  ASSERT_EQ(ErrorOf(workflow_->Remove(kB)), "ok");
  EXPECT_EQ(steps_, Steps({"leave " + std::string(kB), "apply a", "register_others", "announce"}));
  EXPECT_EQ(store_->List().Origins(), Origins({kA}));
}

TEST_F(MyRelaysWorkflowTest, ARelayThatCannotBeReachedIsRemovedAnyway) {
  leave_ok_ = false;
  ASSERT_EQ(ErrorOf(workflow_->Remove(kB)), "ok");
  EXPECT_EQ(store_->List().Origins(), Origins({kA}));
}

TEST_F(MyRelaysWorkflowTest, ThePreferredRelayIsNotRemovableAndTheListNeverEmpty) {
  EXPECT_EQ(ErrorOf(workflow_->Remove(kA)), "relay_preferred_not_removable");
  ASSERT_EQ(ErrorOf(workflow_->Remove(kB)), "ok");
  steps_.clear();
  EXPECT_EQ(ErrorOf(workflow_->Remove(kA)), "relay_list_last") << "mesh-only is not offered in this step";
  EXPECT_TRUE(steps_.empty());
}

TEST_F(MyRelaysWorkflowTest, MakingARelayPreferredRegistersThereAndAnnouncesTheNewOrder) {
  ASSERT_EQ(ErrorOf(workflow_->MakePreferred(kB)), "ok");
  EXPECT_EQ(steps_, Steps({"apply b,a", "register_preferred " + std::string(kB), "register_others", "announce"}));
  EXPECT_EQ(store_->List().Origins(), Origins({kB, kA}));
  const OurRelaysStatus status = workflow_->Status();
  ASSERT_EQ(status.relays.size(), 2u);
  EXPECT_TRUE(status.relays[0].preferred);
  EXPECT_EQ(status.relays[0].registration.state, RelayRegistrationRecord::State::Registered);

  steps_.clear();
  ASSERT_EQ(ErrorOf(workflow_->MakePreferred(kB)), "ok");
  EXPECT_TRUE(steps_.empty()) << "already preferred: nothing to do";
}

TEST_F(MyRelaysWorkflowTest, APreferredRelayThatRefusesStaysPreferredAndSaysWhy) {
  preferred_ok_ = false;
  ASSERT_EQ(ErrorOf(workflow_->MakePreferred(kB)), "ok");
  EXPECT_EQ(store_->List().Preferred(), kB);
  const OurRelaysStatus status = workflow_->Status();
  EXPECT_EQ(status.relays[0].registration.state, RelayRegistrationRecord::State::Failed);
  EXPECT_EQ(status.relays[0].registration.reason, "HTTP 503");
}

TEST_F(MyRelaysWorkflowTest, AChangeTheServicesDoNotFollowIsUndoneAndRefused) {
  // The hub's thread did not take the list in time (or the profile closes): the old clients stay, so does the list,
  // and nothing is registered or announced for a list nobody uses.
  apply_ok_ = false;
  EXPECT_EQ(ErrorOf(workflow_->MakePreferred(kB)), MyRelaysWorkflow::kNotApplied);
  EXPECT_EQ(steps_, Steps({"apply b,a"}));
  EXPECT_EQ(store_->List().Origins(), Origins({kA, kB}));
  EXPECT_EQ(changed_, 1) << "the page reads the list again";
  OurRelaysStore reread(dir_.string());
  EXPECT_EQ(reread.LoadOrSeed(kA, {}).Origins(), Origins({kA, kB})) << "undone in the profile too";
}

TEST_F(MyRelaysWorkflowTest, CloseWaitsForTheChangeInProgressAndRefusesWhatComesAfter) {
  std::promise<void> applying;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  on_apply_ = [&applying, released]() {
    applying.set_value();
    released.wait();
  };
  auto change = std::async(std::launch::async, [this]() { return workflow_->Remove(kB); });
  applying.get_future().wait();
  auto closed = std::async(std::launch::async, [this]() { workflow_->Close(); });
  EXPECT_EQ(closed.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout)
      << "Close waits while a change runs";
  release.set_value();
  EXPECT_EQ(ErrorOf(change.get()), "ok") << "the change in progress finishes";
  closed.get();
  steps_.clear();
  EXPECT_EQ(ErrorOf(workflow_->Add("https://c.example", false)), MyRelaysWorkflow::kClosed);
  EXPECT_EQ(ErrorOf(workflow_->MakePreferred(kA)), MyRelaysWorkflow::kClosed);
  workflow_->FollowCard({kC});
  workflow_->NotePolled(kA, true, 1);
  EXPECT_TRUE(workflow_->Status().relays.empty());
  EXPECT_TRUE(steps_.empty()) << "no port is called once closed";
}

TEST_F(MyRelaysWorkflowTest, OnlyTheMainDeviceChangesTheList) {
  main_device_ = false;
  EXPECT_EQ(ErrorOf(workflow_->Add("https://c.example", false)), "relay_list_not_main_device");
  EXPECT_EQ(ErrorOf(workflow_->Remove(kB)), "relay_list_not_main_device");
  EXPECT_EQ(ErrorOf(workflow_->MakePreferred(kB)), "relay_list_not_main_device");
  EXPECT_TRUE(steps_.empty());
  EXPECT_FALSE(workflow_->Status().editable);
}

TEST_F(MyRelaysWorkflowTest, ALinkedDeviceFollowsTheCardsList) {
  main_device_ = false;
  workflow_->FollowCard({kC, kA});
  EXPECT_EQ(store_->List().Origins(), Origins({kC, kA}));
  EXPECT_EQ(steps_, Steps({"apply c,a", "register_preferred " + std::string(kC), "register_others", "announce"}));
  steps_.clear();
  workflow_->FollowCard({kC, kA});
  workflow_->FollowCard({});
  EXPECT_TRUE(steps_.empty()) << "the same list, or a card naming none: nothing";

  main_device_ = true;
  workflow_->FollowCard({kB});
  EXPECT_EQ(store_->List().Origins(), Origins({kC, kA})) << "the main device's list is its own";
}

TEST_F(MyRelaysWorkflowTest, EachRelaysReachabilityIsItsLastPoll) {
  workflow_->NotePolled(kB, false, 10);
  workflow_->NotePolled(kB, false, 20);
  workflow_->NotePolled(kA, true, 30);
  const OurRelaysStatus status = workflow_->Status();
  EXPECT_EQ(status.relays[0].reach, OurRelayStatus::Reach::Ok);
  EXPECT_EQ(status.relays[1].reach, OurRelayStatus::Reach::Failed);
  EXPECT_EQ(status.relays[1].reach_at_ms, 20);
  EXPECT_EQ(changed_, 2) << "the page reads again on a change of state, not on every poll";
}

TEST_F(MyRelaysWorkflowTest, AnotherRelaysLastRegistrationShowsAsItAnswered) {
  store_->Record(kB, RelayRegistrationRecord{.state = RelayRegistrationRecord::State::Failed, .reason = "HTTP 403"});
  const OurRelaysStatus status = workflow_->Status();
  EXPECT_EQ(status.relays[1].registration.reason, "HTTP 403");
  EXPECT_TRUE(status.editable);
}

} // namespace
} // namespace pbr
