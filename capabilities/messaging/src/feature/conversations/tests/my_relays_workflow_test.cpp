#include "feature/conversations/MyRelaysWorkflow.h"

#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

// Changing my relays (multi-relay R3, MR006, MR008; SERVICE_ENDPOINTS § My relays): each change is kept, the services
// follow it, our registration goes where it is new and leaves where we left, and the card with the new list is
// announced. Only the main device changes the list; a linked device follows the card's.

constexpr const char kA[] = "https://a.example/api/relay";
constexpr const char kB[] = "https://b.example/api/relay";
constexpr const char kC[] = "https://c.example/api/relay";
constexpr const char kIdTextA[] = "relay:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";
constexpr const char kIdTextB[] = "relay:AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE";
constexpr const char kIdTextC[] = "relay:AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgI";
const RelayId kPeerA = RelayId::Parse(kIdTextA).value();
const RelayId kPeerB = RelayId::Parse(kIdTextB).value();
const RelayId kPeerC = RelayId::Parse(kIdTextC).value();
// What a person pastes (MR008): the relay's link, its address with its relay id.
const std::string kLinkC = std::string("https://c.example#") + kIdTextC;

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
    store_->LoadOrSeed(kA, kPeerA, {std::string(kB) + "#" + kIdTextB}, false);
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
        return AppError::Network(Err::Network::HttpError, "HTTP 503");
      }
      registered_at_ = store_->List().Preferred();
      return {};
    };
    ports.preferred_registration = [this]() -> std::optional<RelayRegistrationRecord> {
      if (registered_at_ != store_->List().Preferred()) {
        return RelayRegistrationRecord{.state = RelayRegistrationRecord::State::NotRegistered};
      }
      return RelayRegistrationRecord{.state = RelayRegistrationRecord::State::Registered, .expires_at = "2099"};
    };
    ports.register_others = [this]() { steps_.push_back("register_others"); };
    ports.announce = [this]() { steps_.push_back("announce"); };
    ports.changed = [this]() { ++changed_; };
    ports.check_identity = [this](const std::string& base_url, const RelayId& relay_id) {
      steps_.push_back("identity " + base_url + (relay_id.empty() ? "" : " " + relay_id.ToString().substr(0, 10)));
      AddIdentityCheck check = identity_check_;
      if (check.relay_id.empty()) {
        check.relay_id = relay_id; // it proves the link's
      }
      return check;
    };
    ports.drop_pin = [this](const std::string& origin) { steps_.push_back("drop_pin " + origin); };
    ports.rejoin = [this](const std::string& origin) { steps_.push_back("rejoin " + origin); };
    ports.identity_mismatch = [this](const std::string& origin) { return mismatch_ == origin; };
    ports.relay_id_of = [this](const std::string& origin) {
      const auto it = pinned_.find(origin);
      return it == pinned_.end() ? RelayId{} : it->second;
    };
    ports.org_identity_mismatch = [this]() { return org_mismatch_; };
    ports.now_ms = []() { return int64_t{1'761'235'200'000}; }; // 2025-10-23
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
  AddIdentityCheck identity_check_;
  std::string mismatch_;
  std::string org_mismatch_;
  std::map<std::string, RelayId> pinned_;
};

TEST_F(MyRelaysWorkflowTest, ARelayAddedAnswersIsKeptLastAndRegisteredOnAndTheCardGoes) {
  ASSERT_EQ(ErrorOf(workflow_->Add(std::string("https://C.example#") + kIdTextC, false)), "ok");
  EXPECT_EQ(steps_, Steps({"probe " + std::string(kC), "identity " + std::string(kC) + " relay:AgIC", "apply a,b,c",
                           "register_others", "announce"}))
      << "checked against its link before anything is signed for it (MR008-PIN)";
  EXPECT_EQ(store_->List().Origins(), Origins({kA, kB, kC}));
  EXPECT_EQ(store_->List().relays.back().relay_id, kPeerC) << "kept with the relay id its link names";
  EXPECT_EQ(changed_, 1);
  OurRelaysStore reread(dir_.string());
  EXPECT_EQ(reread.LoadOrSeed(kA, RelayId{}, {}, false).Origins(), Origins({kA, kB, kC})) << "kept in the profile";
}

// MR008 (what the user sees): Add's identity refusals; nothing is kept for a relay that does not answer to its link.
TEST_F(MyRelaysWorkflowTest, ARelayThatDoesNotAnswerToItsLinkIsNotAdded) {
  identity_check_ = AddIdentityCheck{.result = AddIdentityCheck::Result::Unavailable};
  EXPECT_EQ(ErrorOf(workflow_->Add(kLinkC, false)), MyRelaysWorkflow::kIdentityUnavailable);
  identity_check_ = AddIdentityCheck{.result = AddIdentityCheck::Result::Mismatch};
  EXPECT_EQ(ErrorOf(workflow_->Add(kLinkC, false)), MyRelaysWorkflow::kIdentityMismatch);
  identity_check_ = AddIdentityCheck{.result = AddIdentityCheck::Result::Taken, .holder = kB};
  EXPECT_EQ(ErrorOf(workflow_->Add(kLinkC, false)), std::string(MyRelaysWorkflow::kIdentityTaken) + "|b.example")
      << "the relay on our list the link names";
  identity_check_ =
      AddIdentityCheck{.result = AddIdentityCheck::Result::Taken, .holder = "https://org.example/api/relay"};
  EXPECT_EQ(ErrorOf(workflow_->Add(kLinkC, false)), std::string(MyRelaysWorkflow::kIdentityTakenKnown) + "|org.example")
      << "a relay we know (the org's)";
  EXPECT_EQ(store_->List().Origins(), Origins({kA, kB}));
}

// MR008 item 4: a release build takes a relay by its link only; a developer build may add a bare origin, which is
// checked on first use (its certificate's relay id is kept with it).
TEST_F(MyRelaysWorkflowTest, ABareOriginIsRefusedOutsideDeveloperBuilds) {
  EXPECT_EQ(ErrorOf(workflow_->Add("https://c.example", false)), MyRelaysWorkflow::kLinkHasNoId);
  EXPECT_EQ(ErrorOf(workflow_->Add("https://c.example#QmbSD2vbEJTdL9PW5w5mr3pak9EYyoiYfiqCPJJ6AxJBig", false)),
            "relay_origin_bad_relay_id")
      << "a PeerId is no relay id (a link from before the cutover)";
  EXPECT_TRUE(steps_.empty()) << "nothing is asked of it";
  identity_check_ = AddIdentityCheck{.result = AddIdentityCheck::Result::Ok, .relay_id = kPeerC, .pinned_now = true};
  ASSERT_EQ(ErrorOf(workflow_->Add("https://c.example", true)), "ok");
  EXPECT_EQ(steps_[1], "identity " + std::string(kC)) << "no relay id to check against: first use";
  EXPECT_EQ(store_->List().relays.back().relay_id, kPeerC);
}

// MR007, MR008: Remove… drops the pin once the services left the relay; a relay that does not match its link gets no
// revoke (never signed for).
TEST_F(MyRelaysWorkflowTest, RemoveDropsThePinAndRevokesNothingForAMismatch) {
  ASSERT_EQ(ErrorOf(workflow_->Remove(kB)), "ok");
  EXPECT_EQ(steps_.front(), "leave " + std::string(kB));
  EXPECT_EQ(steps_.back(), "drop_pin " + std::string(kB)) << "after the services left it";
  ASSERT_EQ(ErrorOf(workflow_->Add(kLinkC, false)), "ok");
  steps_.clear();
  mismatch_ = kC;
  ASSERT_EQ(ErrorOf(workflow_->Remove(kC)), "ok");
  EXPECT_EQ(steps_.front(), "apply a") << "no revoke";
  EXPECT_EQ(steps_.back(), "drop_pin " + std::string(kC));
}

// MR008: a relay that does not match its link shows so, and the org's origin's mismatch shows on any device; there is
// nothing to accept (a new key is a new address).
TEST_F(MyRelaysWorkflowTest, AMismatchShowsAndEachRelayNamesItsRelayId) {
  mismatch_ = kB;
  org_mismatch_ = "https://org.example/api/relay";
  const OurRelaysStatus status = workflow_->Status();
  EXPECT_FALSE(status.relays[0].identity_mismatch);
  EXPECT_TRUE(status.relays[1].identity_mismatch);
  EXPECT_EQ(status.relays[0].relay_id, kPeerA) << "the org relay's, from the build";
  EXPECT_EQ(status.relays[1].relay_id, kPeerB) << "from its link";
  EXPECT_EQ(status.org_identity_mismatch, org_mismatch_);
  pinned_[kA] = kPeerC; // a newer build names another relay id for the org's relay
  EXPECT_EQ(workflow_->Status().relays[0].relay_id, kPeerC) << "what it is expected to prove now";
}

TEST_F(MyRelaysWorkflowTest, ARelayThatDoesNotAnswerIsNotAdded) {
  reachable_ = false;
  EXPECT_EQ(ErrorOf(workflow_->Add(kLinkC, false)), MyRelaysWorkflow::kUnreachable);
  EXPECT_EQ(steps_, Steps({"probe " + std::string(kC)}));
  EXPECT_EQ(store_->List().Origins(), Origins({kA, kB}));
}

TEST_F(MyRelaysWorkflowTest, WhatIsNotAnHttpsOriginIsRefusedBeforeAnythingIsAsked) {
  EXPECT_EQ(ErrorOf(workflow_->Add("http://c.example", false)), "relay_origin_not_https");
  EXPECT_EQ(ErrorOf(workflow_->Add("https://c.example/inbox", false)), "relay_origin_has_path");
  EXPECT_EQ(ErrorOf(workflow_->Add("https://c.example?x", false)), "relay_origin_has_query");
  EXPECT_EQ(ErrorOf(workflow_->Add(std::string("https://b.example#") + kIdTextB, false)), "relay_already_listed");
  EXPECT_TRUE(steps_.empty());
  EXPECT_EQ(ErrorOf(workflow_->Add("http://c.example", true)), "ok") << "a developer build's plain relay";
}

TEST_F(MyRelaysWorkflowTest, ARelayAtALocalAddressIsRefusedOutsideDeveloperBuilds) {
  for (const char* local : {"https://localhost", "https://127.0.0.1:8443", "https://192.168.1.5", "https://[::1]",
                            "https://10.0.0.2/api/relay"}) {
    EXPECT_EQ(ErrorOf(workflow_->Add(std::string(local) + "#" + kIdTextC, false)), MyRelaysWorkflow::kLocal) << local;
  }
  EXPECT_TRUE(steps_.empty()) << "nothing is asked of it";
  EXPECT_EQ(ErrorOf(workflow_->Add("https://192.168.1.5", true)), "ok") << "a developer build's (the lab's)";
}

TEST_F(MyRelaysWorkflowTest, ARelayRemovedIsLeftFirstThenTheServicesFollow) {
  ASSERT_EQ(ErrorOf(workflow_->Remove(kB)), "ok");
  EXPECT_EQ(steps_, Steps({"leave " + std::string(kB), "apply a", "register_others", "announce",
                           "drop_pin " + std::string(kB)}));
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
  EXPECT_EQ(status.relays[0].registration.reason, "unreachable") << "a code the page words (D3)";
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
  EXPECT_EQ(reread.LoadOrSeed(kA, RelayId{}, {}, false).Origins(), Origins({kA, kB})) << "undone in the profile too";
}

// T3: a Remove the services did not follow is undone: the relay, left already, is ours again and keeps its pin.
TEST_F(MyRelaysWorkflowTest, ARemoveTheServicesDoNotFollowIsUndoneAndTheRelayRejoined) {
  apply_ok_ = false;
  EXPECT_EQ(ErrorOf(workflow_->Remove(kB)), MyRelaysWorkflow::kNotApplied);
  EXPECT_EQ(steps_, Steps({"leave " + std::string(kB), "apply a", "rejoin " + std::string(kB)}));
  EXPECT_EQ(store_->List().Origins(), Origins({kA, kB}));
}

// An Add the services did not follow drops the pin it wrote: a relay not ours is never pinned (MR007).
TEST_F(MyRelaysWorkflowTest, AnAddTheServicesDoNotFollowDropsThePinItWrote) {
  apply_ok_ = false;
  identity_check_ = AddIdentityCheck{.result = AddIdentityCheck::Result::Ok, .pinned_now = true};
  EXPECT_EQ(ErrorOf(workflow_->Add(kLinkC, false)), MyRelaysWorkflow::kNotApplied);
  EXPECT_EQ(steps_.back(), "drop_pin " + std::string(kC));
  steps_.clear();
  identity_check_ = AddIdentityCheck{.result = AddIdentityCheck::Result::Ok, .pinned_now = false};
  EXPECT_EQ(ErrorOf(workflow_->Add(kLinkC, false)), MyRelaysWorkflow::kNotApplied);
  EXPECT_EQ(steps_.back(), "apply a,b,c") << "a pin it found stays";
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
  EXPECT_EQ(ErrorOf(workflow_->Add(kLinkC, false)), MyRelaysWorkflow::kClosed);
  EXPECT_EQ(ErrorOf(workflow_->MakePreferred(kA)), MyRelaysWorkflow::kClosed);
  workflow_->FollowCard({{.origin = kC, .relay_id = kPeerC}});
  workflow_->NotePolled(kA, true, 1);
  EXPECT_TRUE(workflow_->Status().relays.empty());
  EXPECT_TRUE(steps_.empty()) << "no port is called once closed";
}

TEST_F(MyRelaysWorkflowTest, OnlyTheMainDeviceChangesTheList) {
  main_device_ = false;
  EXPECT_EQ(ErrorOf(workflow_->Add(kLinkC, false)), "relay_list_not_main_device");
  EXPECT_EQ(ErrorOf(workflow_->Remove(kB)), "relay_list_not_main_device");
  EXPECT_EQ(ErrorOf(workflow_->MakePreferred(kB)), "relay_list_not_main_device");
  EXPECT_TRUE(steps_.empty());
  EXPECT_FALSE(workflow_->Status().editable);
}

TEST_F(MyRelaysWorkflowTest, ALinkedDeviceFollowsTheCardsList) {
  main_device_ = false;
  const std::vector<RelayAddress> card{{.origin = kC, .relay_id = kPeerC}, {.origin = kA, .relay_id = kPeerA}};
  workflow_->FollowCard(card);
  EXPECT_EQ(store_->List().Origins(), Origins({kC, kA}));
  EXPECT_EQ(store_->List().relays[0].relay_id, kPeerC) << "each with the relay id our own card names (MR008)";
  EXPECT_EQ(steps_, Steps({"apply c,a", "register_preferred " + std::string(kC), "register_others", "announce"}));
  steps_.clear();
  workflow_->FollowCard(card);
  workflow_->FollowCard({});
  EXPECT_TRUE(steps_.empty()) << "the same list, or a card naming none: nothing";

  main_device_ = true;
  workflow_->FollowCard({{.origin = kB, .relay_id = kPeerB}});
  EXPECT_EQ(store_->List().Origins(), Origins({kC, kA})) << "the main device's list is its own";
}

// D2: a restore from the locker brings the person's list back on any device (the restored one is the main device
// with no card yet); registering and the announce are the restore's own steps.
TEST_F(MyRelaysWorkflowTest, ARestoreTakesTheLockersList) {
  main_device_ = false;
  const std::vector<RelayAddress> locker{{.origin = kC, .relay_id = kPeerC}, {.origin = kA, .relay_id = kPeerA}};
  workflow_->Restore(locker);
  EXPECT_EQ(store_->List().Origins(), Origins({kC, kA}));
  EXPECT_EQ(store_->List().relays[0].relay_id, kPeerC);
  EXPECT_EQ(steps_, Steps({"apply c,a", "register_others"})) << "no announce, no preferred registration of its own";
  steps_.clear();
  workflow_->Restore(locker);
  EXPECT_TRUE(steps_.empty()) << "the same list: nothing";
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

// D4: never "Registering…" forever: a preferred relay the identity holds no registration on reads NotRegistered and
// the others wait for it; another relay's registration whose end passed reads Expired.
TEST_F(MyRelaysWorkflowTest, ARegistrationMissingOrEndedSaysSo) {
  registered_at_.clear();
  OurRelaysStatus status = workflow_->Status();
  EXPECT_EQ(status.relays[0].registration.state, RelayRegistrationRecord::State::NotRegistered);
  EXPECT_EQ(status.relays[1].registration.state, RelayRegistrationRecord::State::NotRegistered)
      << "it registers after the preferred relay";

  registered_at_ = kA;
  store_->Record(kB, RelayRegistrationRecord{.state = RelayRegistrationRecord::State::Registered,
                                             .expires_at = "2025-01-01T00:00:00.000Z",
                                             .at_ms = 1});
  status = workflow_->Status();
  EXPECT_EQ(status.relays[0].registration.state, RelayRegistrationRecord::State::Registered);
  EXPECT_EQ(status.relays[1].registration.state, RelayRegistrationRecord::State::Expired);

  registered_at_.clear();
  store_->Record(kA, RelayRegistrationRecord{.state = RelayRegistrationRecord::State::Failed, .reason = "refused"});
  EXPECT_EQ(workflow_->Status().relays[0].registration.state, RelayRegistrationRecord::State::Failed)
      << "a failure shows while the identity holds none there";
}

TEST_F(MyRelaysWorkflowTest, AnotherRelaysLastRegistrationShowsAsItAnswered) {
  store_->Record(kB, RelayRegistrationRecord{.state = RelayRegistrationRecord::State::Failed, .reason = "HTTP 403"});
  const OurRelaysStatus status = workflow_->Status();
  EXPECT_EQ(status.relays[1].registration.reason, "HTTP 403");
  EXPECT_TRUE(status.editable);
}

} // namespace
} // namespace pbr
