// DEV/PLAN-call-failed-redial.md: a 1:1 call that never connected, on two phones' call screens (CallPresenter) over
// two real call stacks whose call control the test carries. "Call again" ends the failed call and calls the same person
// again (1); each phone's "Couldn't reach them" page is its own (2).
//
// Media runs with its devices skipped (SetSkipDeviceOpenForTest): no microphone or camera is opened.
#include "feature/presenters/CallPresenter.h"

#include "feature/calls/tests/call_stack_compose_support.h"
#include "common/media/MediaSessionsOwner.h"

#include <gtest/gtest.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbr {
namespace {

using test::BuildStackSide;
using test::DestroyStackSide;
using test::SoftStopStackSide;
using test::StackSide;

/** One phone: its call stack, its call screen, the pills it was told to show. */
struct Phone {
  StackSide side;
  std::unique_ptr<CallPresenter> presenter;
  std::vector<CallNotice> notices;
  /** Call control on its way to this phone. */
  std::deque<ThreadMessage> inbox;
  /** Messages to this phone the "relay" keeps back while this returns true. */
  std::function<bool(const ThreadMessage&)> hold;
  std::deque<ThreadMessage> held;
  /** The DM with the other phone (its id is what the stack files that peer's calls under). */
  std::string thread_id;
  int64_t steady_ms = 1'000'000;

  const CallScreen& Screen() const { return presenter->Screen(); }
  bool Told(const CallNotice::Kind kind) const {
    for (const CallNotice& n : notices) {
      if (n.kind == kind) {
        return true;
      }
    }
    return false;
  }
};

bool IsControl(const ThreadMessage& msg, const char* type) {
  return msg.payload_json.find(type) != std::string::npos;
}

class CallFailedRedialTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    AppRuntime::Initialize(MediaSessionsManualRuntimeConfig());
    AppRuntime::InitializeUI();
    a_.side.on_send = [this](const std::string&, const ThreadMessage& msg) {
      sent_by_a_.push_back(msg);
      b_.inbox.push_back(msg);
    };
    b_.side.on_send = [this](const std::string&, const ThreadMessage& msg) {
      sent_by_b_.push_back(msg);
      a_.inbox.push_back(msg);
    };
    BuildStackSide(a_.side, "redial_a", 0xa0);
    BuildStackSide(b_.side, "redial_b", 0xb0);
    a_.side.dial->connected[b_.side.local_identity] = true;
    a_.side.dial->endpoints[b_.side.local_identity] = "/ip4/127.0.0.1/udp/47101/adp/1.0.0/p2p/12D3KooWB";
    b_.side.dial->connected[a_.side.local_identity] = true;
    b_.side.dial->endpoints[a_.side.local_identity] = "/ip4/127.0.0.1/udp/47100/adp/1.0.0/p2p/12D3KooWA";
    // A dial from one phone lands on the other's inbound handler (as in the dual-stack compose test).
    Wire(a_, b_);
    Wire(b_, a_);
    for (auto [self, other] : {std::pair{&a_, &b_}, std::pair{&b_, &a_}}) {
      Thread thread;
      thread.id = "dm:" + other->side.local_identity; // the stack's resolve_peer_thread
      thread.kind = ThreadKind::Direct;
      thread.title = "Peer";
      thread.peer_identity_value = other->side.local_identity;
      thread.updated_at = util::NowUnixMs();
      ASSERT_TRUE(self->side.store->UpsertThread(thread));
      self->thread_id = thread.id;
      BindPresenter(*self);
    }
  }

  void TearDown() override {
    a_.presenter.reset();
    b_.presenter.reset();
    SoftStopStackSide(a_.side);
    SoftStopStackSide(b_.side);
    AppRuntime::ShutdownUI();
    AppRuntime::Shutdown();
    DestroyStackSide(a_.side);
    DestroyStackSide(b_.side);
  }

  static void Wire(Phone& dialer, Phone& peer) {
    dialer.side.transport->peer_inbound = [&dialer, &peer](CallMediaDirectConnectParams params) {
      if (!peer.side.transport || !peer.side.transport->inbound.Installed() || peer.side.transport->fail_connects) {
        return;
      }
      params.peer_key = dialer.side.local_identity;
      peer.side.transport->active = true;
      peer.side.transport->active_params = params;
      peer.side.transport->inbound.DeliverThen(std::move(params),
                                               [](CallMediaDirectConnectParams, CallMediaDirectCallbacks cbs) {
                                                 if (cbs.on_connected) {
                                                   cbs.on_connected();
                                                 }
                                               });
    };
  }

  void BindPresenter(Phone& phone) {
    phone.presenter = std::make_unique<CallPresenter>();
    CallFunctionalPorts ports;
    ports.initialized = [this, &phone]() { return &phone == &a_ ? a_unlocked_ : true; };
    ports.backend = [&phone]() { return phone.side.ui.get(); };
    ports.local_relay_identity = [&phone]() { return std::optional<std::string>(phone.side.local_identity); };
    ports.get_thread = [&phone](const std::string& id) { return phone.side.store->GetThread(id); };
    CallPresenter::Callbacks callbacks;
    callbacks.notify = [&phone](const CallNotice& notice) { phone.notices.push_back(notice); };
    phone.presenter->BindCallbacks(std::move(callbacks));
    phone.presenter->BindPorts(std::move(ports));
    phone.presenter->SetSteadyClockForTest([&phone]() { return phone.steady_ms; });
  }

  /** Every call key a phone made, given to the other (the tests' stand-in for the wrapped key). */
  void ShareKeys() {
    for (auto [from, to] : {std::pair{&a_, &b_}, std::pair{&b_, &a_}}) {
      for (const std::string& call_id : {from->presenter->ActiveCallId(), from->side.ui->ActiveCallId()}) {
        if (call_id.empty() || shared_.count(call_id)) {
          continue;
        }
        auto key = from->side.stack->MediaKeys()->LoadEpochKey(call_id, 1);
        if (key && key->has_value()) {
          ASSERT_TRUE(to->side.stack->MediaKeys()->PutEpochKey(call_id, 1, **key));
          shared_.insert(call_id);
        }
      }
    }
  }

  /** The call control waiting for each phone is delivered (except what it holds back), and each screen refreshed. */
  void Pump(int rounds = 8) {
    for (int r = 0; r < rounds; ++r) {
      AppRuntime::RunUIAndOwnerTasks();
      ShareKeys();
      bool moved = false;
      for (auto [to, from] : {std::pair{&b_, &a_}, std::pair{&a_, &b_}}) {
        while (!to->inbox.empty()) {
          ThreadMessage msg = to->inbox.front();
          to->inbox.pop_front();
          if (to->hold && to->hold(msg)) {
            to->held.push_back(std::move(msg));
            continue;
          }
          ASSERT_TRUE(to->side.Deliver(msg, from->side.local_identity));
          moved = true;
        }
      }
      AppRuntime::RunUIAndOwnerTasks();
      a_.presenter->Tick();
      b_.presenter->Tick();
      a_.presenter->Refresh();
      b_.presenter->Refresh();
      if (!moved) {
        break;
      }
    }
  }

  void PumpUntil(const std::function<bool()>& done, int max_ms = 6000) {
    for (int i = 0; i < std::max(1, max_ms / 10); ++i) {
      Pump();
      if (done()) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Pump();
  }

  /** The held messages to `phone` go on their way now. */
  void Release(Phone& phone) {
    phone.hold = {};
    while (!phone.held.empty()) {
      phone.inbox.push_front(phone.held.back());
      phone.held.pop_back();
    }
  }

  /** `caller` calls `callee`, which rings: the call id. */
  std::string Ring(Phone& caller, Phone& callee, const bool video) {
    EXPECT_TRUE(caller.presenter->CallFromChat(caller.thread_id, video));
    PumpUntil([&]() { return callee.Screen().kind == CallScreen::Kind::Ringing; });
    EXPECT_EQ(callee.Screen().kind, CallScreen::Kind::Ringing);
    return callee.Screen().ring_call_id;
  }

  /** A call from `caller` that `callee` answered and that could not connect on either phone: the call id. */
  std::string FailedCall(Phone& caller, Phone& callee, const bool video = false) {
    a_.side.transport->fail_connects = true;
    b_.side.transport->fail_connects = true;
    const std::string call_id = Ring(caller, callee, video);
    EXPECT_FALSE(call_id.empty());
    EXPECT_EQ(callee.presenter->Accept(/*voice_only=*/!video, call_id), call_id);
    PumpUntil([&]() {
      return caller.side.ui->Phase() != CallPhase::OutboundCalling && callee.side.ui->Phase() != CallPhase::Accepting;
    });
    for (Phone* phone : {&caller, &callee}) {
      phone->side.stack->RunOnOwner(
          [call_id](CallDevice& calls) { calls.LiveCallsForTest().NoteMediaFailed(call_id); });
    }
    PumpUntil([&]() { return OffersRedial(caller.Screen()) && OffersRedial(callee.Screen()); });
    EXPECT_TRUE(OffersRedial(caller.Screen())) << CallScreenStatusName(caller.Screen().status);
    EXPECT_TRUE(OffersRedial(callee.Screen())) << CallScreenStatusName(callee.Screen().status);
    a_.side.transport->fail_connects = false;
    b_.side.transport->fail_connects = false;
    return call_id;
  }

  /** Call invites `phone` sent since `from` (an index into its sent list). */
  static std::vector<std::string> InvitesSent(const std::vector<ThreadMessage>& sent, size_t from = 0) {
    std::vector<std::string> out;
    for (size_t i = from; i < sent.size(); ++i) {
      if (IsControl(sent[i], "call_invite")) {
        out.push_back(sent[i].payload_json);
      }
    }
    return out;
  }

  static const LiveCall* Live(Phone& phone, const std::string& call_id) {
    return phone.side.stack->Device()->Live().Find(call_id);
  }

  Phone a_;
  Phone b_;
  std::vector<ThreadMessage> sent_by_a_;
  std::vector<ThreadMessage> sent_by_b_;
  std::set<std::string> shared_;
  bool a_unlocked_ = true;
};

// --- 1. Call again ----------------------------------------------------------------------------------------------------

TEST(CallRedialRuleTest, OnlyAOneToOneCallThatCouldNotConnectOffersCallAgain) {
  CallScreen failed;
  failed.kind = CallScreen::Kind::InCall;
  failed.status = CallScreen::Status::CouldntConnect;
  EXPECT_TRUE(OffersRedial(failed));
  CallScreen group = failed;
  group.group = true;
  EXPECT_FALSE(OffersRedial(group)) << "a group's failed page keeps Try again / Close";
  CallScreen dropped = failed;
  dropped.status = CallScreen::Status::Disconnected;
  EXPECT_FALSE(OffersRedial(dropped)) << "the call-dropped page is not changed";
  for (const CallScreen::Status status : {CallScreen::Status::Calling, CallScreen::Status::Connecting,
                                          CallScreen::Status::Connected, CallScreen::Status::Reconnecting}) {
    CallScreen other = failed;
    other.status = status;
    EXPECT_FALSE(OffersRedial(other)) << CallScreenStatusName(status);
  }
  EXPECT_FALSE(OffersRedial(CallScreen{}));
}

// Video this device cannot send goes voice, as from the chat (the engine here reports the host's encoder).
TEST(CallRedialRuleTest, AVideoCallAgainGoesVoiceWithoutAVideoEncoder) {
  EXPECT_FALSE(ChatCallAsksVideo(/*call_joinable=*/false, /*want_video=*/true, /*video_available=*/false));
  EXPECT_TRUE(ChatCallAsksVideo(false, true, true));
}

TEST_F(CallFailedRedialTest, CallAgainOnAFailedVoiceCallEndsItThenCallsTheSamePersonByVoice) {
  const std::string failed = FailedCall(a_, b_);
  const size_t sent_before = sent_by_a_.size();
  a_.presenter->Redial();
  PumpUntil([&]() { return !a_.presenter->ActiveCallId().empty() && a_.presenter->ActiveCallId() != failed; });

  const std::string redialed = a_.presenter->ActiveCallId();
  ASSERT_FALSE(redialed.empty());
  EXPECT_NE(redialed, failed) << "a new call, not the failed one again";
  ASSERT_NE(Live(a_, failed), nullptr);
  EXPECT_EQ(Live(a_, failed)->State(), LiveCallState::Ended);
  EXPECT_EQ(Live(a_, failed)->EndReason(), LiveCallEndReason::LocalLeave) << "the failed call was ended here";
  // On the wire: the failed call's leave goes before the new invite.
  size_t leave_at = sent_by_a_.size();
  size_t invite_at = sent_by_a_.size();
  for (size_t i = sent_before; i < sent_by_a_.size(); ++i) {
    if (IsControl(sent_by_a_[i], "call_leave") && sent_by_a_[i].payload_json.find(failed) != std::string::npos &&
        leave_at == sent_by_a_.size()) {
      leave_at = i;
    }
    if (IsControl(sent_by_a_[i], "call_invite") && invite_at == sent_by_a_.size()) {
      invite_at = i;
    }
  }
  EXPECT_LT(leave_at, invite_at) << "leave, then call";
  EXPECT_EQ(a_.Screen().kind, CallScreen::Kind::InCall);
  EXPECT_EQ(a_.Screen().call_id, redialed);
  EXPECT_TRUE(a_.Screen().placed_here) << "the caller's calling page";
  EXPECT_EQ(a_.Screen().thread_id, a_.thread_id) << "the same person";
  EXPECT_FALSE(a_.Screen().video) << "voice, as before";
}

TEST_F(CallFailedRedialTest, CallAgainOnAFailedVideoCallCallsByVideoWhenThisDeviceSendsVideo) {
  const std::string failed = FailedCall(a_, b_, /*video=*/true);
  ASSERT_TRUE(a_.Screen().video);
  a_.presenter->Redial();
  PumpUntil([&]() { return !a_.presenter->ActiveCallId().empty() && a_.presenter->ActiveCallId() != failed; });
  ASSERT_NE(a_.presenter->ActiveCallId(), failed);
  // The page's own kind (never the chat's "a call goes on here" rule, which would read the failed call).
  EXPECT_EQ(a_.Screen().video, a_.presenter->VideoCallAvailable());
  EXPECT_EQ(a_.presenter->VideoIntentCallId() == a_.presenter->ActiveCallId(), a_.presenter->VideoCallAvailable());
}

TEST_F(CallFailedRedialTest, TheCalleesCallAgainCallsTheCaller) {
  const std::string failed = FailedCall(a_, b_);
  b_.presenter->Redial();
  PumpUntil([&]() { return a_.Screen().kind == CallScreen::Kind::Ringing; });
  EXPECT_EQ(a_.Screen().kind, CallScreen::Kind::Ringing) << "the caller of the failed call rings now";
  EXPECT_NE(a_.Screen().ring_call_id, failed);
  EXPECT_EQ(a_.Screen().caller_identity, b_.side.local_identity);
}

TEST_F(CallFailedRedialTest, CallAgainDoesNothingWithNoCall) {
  a_.presenter->Redial();
  Pump();
  EXPECT_TRUE(InvitesSent(sent_by_a_).empty());
  EXPECT_EQ(a_.Screen().kind, CallScreen::Kind::None);
}

TEST_F(CallFailedRedialTest, CallAgainDoesNothingWhileCalling) {
  const std::string call_id = Ring(a_, b_, false);
  ASSERT_EQ(a_.presenter->ActiveCallId(), call_id);
  const size_t sent = sent_by_a_.size();
  a_.presenter->Redial();
  Pump();
  EXPECT_EQ(a_.presenter->ActiveCallId(), call_id);
  EXPECT_TRUE(InvitesSent(sent_by_a_, sent).empty());
  EXPECT_NE(Live(a_, call_id)->State(), LiveCallState::Ended) << "still calling";
}

TEST_F(CallFailedRedialTest, CallAgainDoesNothingInACallThatConnected) {
  const std::string call_id = Ring(a_, b_, false);
  ASSERT_EQ(b_.presenter->Accept(true, call_id), call_id);
  PumpUntil([&]() { return a_.side.ui->Phase() == CallPhase::InCall && b_.side.ui->Phase() == CallPhase::InCall; });
  ASSERT_EQ(a_.side.ui->Phase(), CallPhase::InCall);
  ASSERT_FALSE(OffersRedial(a_.Screen()));
  const size_t sent = sent_by_a_.size();
  a_.presenter->Redial();
  Pump();
  EXPECT_EQ(a_.presenter->ActiveCallId(), call_id);
  EXPECT_TRUE(InvitesSent(sent_by_a_, sent).empty());
}

TEST_F(CallFailedRedialTest, TryLaterEndsTheFailedCallAndClosesThePageAsCloseDid) {
  const std::string failed = FailedCall(a_, b_);
  EXPECT_EQ(a_.presenter->Leave(), failed); // "Try later" is the old Close's End
  Pump();
  EXPECT_EQ(a_.Screen().kind, CallScreen::Kind::None);
  EXPECT_FALSE(a_.presenter->FailedPage().has_value());
  EXPECT_EQ(Live(a_, failed)->State(), LiveCallState::Ended);
  EXPECT_TRUE(InvitesSent(sent_by_a_, sent_by_a_.size()).empty());
}

TEST_F(CallFailedRedialTest, CallAgainPressedTwiceCallsOnce) {
  const std::string failed = FailedCall(a_, b_);
  const size_t sent = sent_by_a_.size();
  a_.presenter->Redial();
  a_.presenter->Redial();
  PumpUntil([&]() { return !a_.presenter->ActiveCallId().empty() && a_.presenter->ActiveCallId() != failed; });
  a_.presenter->Redial(); // the calling page is up: nothing
  Pump();
  EXPECT_EQ(InvitesSent(sent_by_a_, sent).size(), 1u) << "one new call";
}

} // namespace
} // namespace pbr
