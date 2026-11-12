// DEV/PLAN-call-failed-redial.md: a 1:1 call that never connected, on two phones' call screens (CallPresenter) over
// two real call stacks whose call control the test carries. "Call again" ends the failed call and calls the same person
// again (1); each phone's "Couldn't reach them" page is its own (2).
//
// Media runs with its devices skipped (SetSkipDeviceOpenForTest): no microphone or camera is opened.
#include "feature/presenters/CallPresenter.h"

#include "feature/calls/tests/call_stack_compose_support.h"
#include "feature/presenters/CallUiAttachGuard.h"
#include "foundation/runtime/AppLifecycle.h"
#include "lib/calls/CallControlCodec.h"
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

/** A hang-up's call control: the leave and the ended that go with it. */
bool IsCallEnd(const ThreadMessage& msg) {
  return IsControl(msg, "call_leave") || IsControl(msg, "call_ended");
}

class CallFailedRedialTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    AppRuntime::Initialize(MediaSessionsManualRuntimeConfig());
    AppRuntime::InitializeUI();
    // Call control for the other phone goes into its inbox; anything for someone else goes nowhere.
    a_.side.on_send = [this](const std::string& peer, const ThreadMessage& msg) {
      sent_by_a_.push_back(msg);
      if (peer == b_.side.local_identity) {
        b_.inbox.push_back(msg);
      }
    };
    b_.side.on_send = [this](const std::string& peer, const ThreadMessage& msg) {
      sent_by_b_.push_back(msg);
      if (peer == a_.side.local_identity) {
        a_.inbox.push_back(msg);
      }
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
    ports.backend = [this, &phone]() { return &phone == &a_ && a_backend_ ? a_backend_ : phone.side.ui.get(); };
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

  /** A call from `caller` that `callee` answered, its steps still going (it has not connected, nor failed yet). */
  std::string AnsweredNotConnected(Phone& caller, Phone& callee) {
    a_.side.transport->fail_connects = true;
    b_.side.transport->fail_connects = true;
    const std::string call_id = Ring(caller, callee, false);
    EXPECT_EQ(callee.presenter->Accept(/*voice_only=*/true, call_id), call_id);
    PumpUntil([&]() {
      return caller.Screen().kind == CallScreen::Kind::InCall &&
             caller.Screen().status != CallScreen::Status::Calling && callee.Screen().kind == CallScreen::Kind::InCall;
    });
    for (Phone* phone : {&caller, &callee}) {
      EXPECT_EQ(phone->Screen().kind, CallScreen::Kind::InCall);
      EXPECT_FALSE(phone->Screen().was_connected);
      EXPECT_FALSE(OffersRedial(phone->Screen())) << "not failed yet: " << CallScreenStatusName(phone->Screen().status);
    }
    return call_id;
  }

  /** An invite to `to` from `sender` (any account), as its call control would bring it. */
  Roe<void> InviteFrom(Phone& to, const std::string& sender, const std::string& call_id,
                       std::optional<std::string> group_id = std::nullopt) {
    CallInviteDetail invite;
    invite.call_id = call_id;
    invite.inviter_identity = sender;
    invite.invitee_identity = to.side.local_identity;
    invite.media_mode = CallMediaMode::Voice;
    invite.origin_group_id = std::move(group_id);
    invite.media_epoch = 1;
    invite.media_key_id = "mk:1";
    invite.expires_at = util::NowUnixMs() + kDefaultCallInviteTtlMs;
    auto detail = CallControlCodec::EncodeInvite(invite);
    if (!detail) {
      return detail.error();
    }
    if (auto key = to.side.stack->MediaKeys()->PutEpochKey(call_id, 1, ByteVector(32, 0x5a)); !key) {
      return key.error();
    }
    auto msg = CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallInvite, "call control",
                                                    *detail, sender);
    if (!msg) {
      return msg.error();
    }
    return to.side.Deliver(*msg, sender);
  }

  /** The kept page, or none (FailedPage) — and with it, nothing of a call open on `phone`. */
  void ExpectNoCallOpen(Phone& phone) {
    EXPECT_TRUE(phone.presenter->ActiveCallId().empty()) << "no call on screen (hold, Android's call service)";
    EXPECT_EQ(phone.side.ui->Phase(), CallPhase::Idle) << "the stack in no call (ringback, background work)";
    EXPECT_FALSE(phone.side.stack->Engine()->IsActive()) << "no media: mic, camera, audio session";
    EXPECT_FALSE(OngoingCallWanted(
        !phone.presenter->ActiveCallId().empty(),
        EngineHasOpenCall(phone.side.ui->Available(), phone.side.ui->ActiveCallId(), phone.side.ui->Phase())))
        << "the phone's ongoing-call notification is off";
    EXPECT_FALSE(AppLifecycle::LiveSessionOn()) << "no call holding the app awake in the background";
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
  /** Set: phone A's screen reads another call stack (another account's). */
  CallUiBackend* a_backend_ = nullptr;
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

// --- 2. Each phone's failed page is its own -------------------------------------------------------------------------

TEST(CallFailedPageRuleTest, ThePeerEndingACallThatNeverConnectedKeepsItsPage) {
  CallScreen failed;
  failed.kind = CallScreen::Kind::InCall;
  failed.status = CallScreen::Status::CouldntConnect;
  failed.call_id = "c1";
  failed.thread_id = "dm:peer";
  failed.peer_identity = "peer";
  EXPECT_TRUE(KeepsFailedPage(failed, "c1", false));
  EXPECT_FALSE(KeepsFailedPage(failed, "c1", /*declined=*/true)) << "a decline is told as before";
  EXPECT_FALSE(KeepsFailedPage(failed, "c0", false)) << "another call";
  CallScreen connecting = failed;
  connecting.status = CallScreen::Status::Connecting;
  EXPECT_TRUE(KeepsFailedPage(connecting, "c1", false)) << "answered, the steps still going";
  CallScreen ringing_out = failed;
  ringing_out.status = CallScreen::Status::Calling;
  ringing_out.placed_here = true;
  EXPECT_FALSE(KeepsFailedPage(ringing_out, "c1", false)) << "still ringing them";
  CallScreen connected = connecting;
  connected.was_connected = true;
  EXPECT_FALSE(KeepsFailedPage(connected, "c1", false)) << "a call that connected";
  CallScreen dropped = connected;
  dropped.status = CallScreen::Status::Disconnected;
  EXPECT_FALSE(KeepsFailedPage(dropped, "c1", false));
  CallScreen group = failed;
  group.group = true;
  EXPECT_FALSE(KeepsFailedPage(group, "c1", false)) << "a group's call";
  CallScreen answering;
  answering.kind = CallScreen::Kind::Answering;
  answering.ring_call_id = "c1";
  answering.caller_identity = "peer";
  answering.thread_id = "dm:peer";
  EXPECT_TRUE(KeepsFailedPage(answering, "c1", false)) << "answered here, not in the call yet";
  CallScreen ring = answering;
  ring.kind = CallScreen::Kind::Ringing;
  EXPECT_FALSE(KeepsFailedPage(ring, "c1", false)) << "a ring not answered: a missed call, as before";

  failed.video = true;
  failed.hint_missing_mic = true;
  const CallFailedPage page = FailedPageOf(failed);
  EXPECT_EQ(page.call_id, "c1");
  EXPECT_EQ(page.thread_id, "dm:peer");
  EXPECT_EQ(page.peer_identity, "peer");
  EXPECT_TRUE(page.video);
  EXPECT_TRUE(page.hint_missing_mic);
  EXPECT_EQ(FailedPageOf(answering).peer_identity, "peer");
}

TEST(CallFailedPageRuleTest, OnlyTheFailedCallsOwnPeerCallingAgainEndsIt) {
  CallScreen failed;
  failed.kind = CallScreen::Kind::InCall;
  failed.status = CallScreen::Status::CouldntConnect;
  failed.call_id = "c1";
  failed.thread_id = "dm:peer";
  PendingCallInvite again;
  again.call_id = "c2";
  again.inviter_identity = "peer";
  again.origin_thread_id = "dm:peer";
  EXPECT_TRUE(EndsFailedCallForRing(failed, "c1", again, "peer", false));
  EXPECT_FALSE(EndsFailedCallForRing(failed, "c1", again, "peer", /*redial_pending=*/true))
      << "our own Call again is on its way: the stack merges the two";
  PendingCallInvite same_call = again;
  same_call.call_id = "c1";
  EXPECT_FALSE(EndsFailedCallForRing(failed, "c1", same_call, "peer", false)) << "the old invite delivered again";
  PendingCallInvite group = again;
  group.origin_group_id = "g1";
  EXPECT_FALSE(EndsFailedCallForRing(failed, "c1", group, "peer", false)) << "the same person's group call";
  PendingCallInvite other = again;
  other.inviter_identity = "carol";
  EXPECT_FALSE(EndsFailedCallForRing(failed, "c1", other, "peer", false)) << "someone else";
  PendingCallInvite other_chat = again;
  other_chat.origin_thread_id = "dm:other";
  EXPECT_FALSE(EndsFailedCallForRing(failed, "c1", other_chat, "peer", false));
  CallScreen connecting = failed;
  connecting.status = CallScreen::Status::Connecting;
  EXPECT_FALSE(EndsFailedCallForRing(connecting, "c1", again, "peer", false)) << "a call still connecting";
  CallScreen group_page = failed;
  group_page.group = true;
  EXPECT_FALSE(EndsFailedCallForRing(group_page, "c1", again, "peer", false));
  EXPECT_FALSE(EndsFailedCallForRing(failed, "c9", again, "peer", false)) << "the page is not the call open";
}

TEST_F(CallFailedRedialTest, ThePeerEndingACallAnsweredHereThatNeverConnectedLeavesTheFailedPage) {
  const std::string call_id = AnsweredNotConnected(a_, b_);
  b_.notices.clear(); // its ring's
  (void)a_.presenter->Leave();
  PumpUntil([&]() { return b_.side.ui->Phase() == CallPhase::Idle; });
  EXPECT_EQ(b_.Screen().kind, CallScreen::Kind::None);
  ASSERT_TRUE(b_.presenter->FailedPage().has_value()) << "the page stays (not gone with a pill)";
  EXPECT_EQ(b_.presenter->FailedPage()->call_id, call_id);
  EXPECT_EQ(b_.presenter->FailedPage()->peer_identity, a_.side.local_identity);
  EXPECT_EQ(b_.presenter->FailedPage()->thread_id, b_.thread_id);
  EXPECT_FALSE(b_.Told(CallNotice::Kind::PeerEnded));
  EXPECT_TRUE(b_.notices.empty()) << "no pill at all";
  ExpectNoCallOpen(b_);
}

TEST_F(CallFailedRedialTest, TheCalleeEndingACallItAnsweredLeavesTheCallersFailedPage) {
  (void)AnsweredNotConnected(a_, b_);
  (void)b_.presenter->Leave();
  PumpUntil([&]() { return a_.side.ui->Phase() == CallPhase::Idle; });
  ASSERT_TRUE(a_.presenter->FailedPage().has_value());
  EXPECT_EQ(a_.presenter->FailedPage()->peer_identity, b_.side.local_identity);
  EXPECT_TRUE(a_.notices.empty());
  ExpectNoCallOpen(a_);
}

TEST_F(CallFailedRedialTest, ARingNotAnsweredThatTheCallerCancelsGoesAsBefore) {
  (void)Ring(a_, b_, false);
  (void)a_.presenter->Leave();
  PumpUntil([&]() { return b_.Screen().kind == CallScreen::Kind::None; });
  EXPECT_EQ(b_.Screen().kind, CallScreen::Kind::None);
  EXPECT_FALSE(b_.presenter->FailedPage().has_value());
}

TEST_F(CallFailedRedialTest, ThePeerEndingAFailedCallLeavesTheSamePageWithNoPill) {
  const std::string call_id = FailedCall(a_, b_, /*video=*/true);
  const CallScreen before = b_.Screen();
  b_.notices.clear();
  (void)a_.presenter->Leave(); // their Try later
  PumpUntil([&]() { return b_.side.ui->Phase() == CallPhase::Idle; });
  EXPECT_EQ(b_.Screen().kind, CallScreen::Kind::None) << "no call on screen";
  ASSERT_TRUE(b_.presenter->FailedPage().has_value());
  const CallFailedPage& page = *b_.presenter->FailedPage();
  EXPECT_EQ(page.call_id, call_id);
  EXPECT_EQ(page.thread_id, before.thread_id);
  EXPECT_EQ(page.peer_identity, before.peer_identity);
  EXPECT_EQ(page.video, before.video);
  EXPECT_EQ(page.hint_missing_mic, before.hint_missing_mic);
  EXPECT_EQ(page.hint_seed_unreachable, before.hint_seed_unreachable);
  EXPECT_TRUE(b_.notices.empty()) << "no ended-by-them pill, no couldn't-connect pill";
  ExpectNoCallOpen(b_);
  Pump();
  EXPECT_TRUE(b_.presenter->FailedPage().has_value()) << "it stays until tapped";
}

TEST_F(CallFailedRedialTest, TryLaterOnTheKeptPageClosesItAndSendsNothing) {
  (void)FailedCall(a_, b_);
  (void)a_.presenter->Leave();
  PumpUntil([&]() { return b_.presenter->FailedPage().has_value(); });
  ASSERT_TRUE(b_.presenter->FailedPage().has_value());
  const size_t sent = sent_by_b_.size();
  EXPECT_EQ(b_.presenter->Leave(), "") << "no call to end";
  Pump();
  EXPECT_FALSE(b_.presenter->FailedPage().has_value());
  EXPECT_EQ(sent_by_b_.size(), sent) << "nothing sent";
}

TEST_F(CallFailedRedialTest, CallAgainOnTheKeptPageCallsWithoutALeave) {
  (void)FailedCall(a_, b_, /*video=*/true);
  (void)a_.presenter->Leave();
  PumpUntil([&]() { return b_.presenter->FailedPage().has_value(); });
  ASSERT_TRUE(b_.presenter->FailedPage().has_value());
  const size_t sent = sent_by_b_.size();
  b_.presenter->Redial();
  PumpUntil([&]() { return !b_.presenter->ActiveCallId().empty(); });
  EXPECT_FALSE(b_.presenter->FailedPage().has_value());
  EXPECT_EQ(b_.Screen().kind, CallScreen::Kind::InCall) << "its calling page";
  EXPECT_TRUE(b_.Screen().placed_here);
  EXPECT_EQ(b_.Screen().video, b_.presenter->VideoCallAvailable()) << "video, as the page was";
  EXPECT_EQ(InvitesSent(sent_by_b_, sent).size(), 1u);
  for (size_t i = sent; i < sent_by_b_.size(); ++i) {
    EXPECT_FALSE(IsControl(sent_by_b_[i], "call_leave")) << "no call to leave first";
  }
  PumpUntil([&]() { return a_.Screen().kind == CallScreen::Kind::Ringing; });
  EXPECT_EQ(a_.Screen().kind, CallScreen::Kind::Ringing) << "a plain call on their side";
}

TEST_F(CallFailedRedialTest, TheSamePersonCallingOverTheKeptPageRingsWithNoConflict) {
  (void)FailedCall(a_, b_);
  (void)a_.presenter->Leave();
  PumpUntil([&]() { return b_.presenter->FailedPage().has_value(); });
  ASSERT_TRUE(a_.presenter->CallFromChat(a_.thread_id, false));
  PumpUntil([&]() { return b_.Screen().kind == CallScreen::Kind::Ringing; });
  EXPECT_EQ(b_.Screen().kind, CallScreen::Kind::Ringing);
  EXPECT_FALSE(b_.Screen().conflict) << "no call here to end";
  EXPECT_FALSE(b_.presenter->FailedPage().has_value()) << "the ring takes the page's place";
}

TEST_F(CallFailedRedialTest, SomeoneElseCallingOverTheKeptPageRingsWithNoConflict) {
  (void)FailedCall(a_, b_);
  (void)a_.presenter->Leave();
  PumpUntil([&]() { return b_.presenter->FailedPage().has_value(); });
  ASSERT_TRUE(InviteFrom(b_, "account:carol", "call:carol-1"));
  PumpUntil([&]() { return b_.Screen().kind == CallScreen::Kind::Ringing; });
  EXPECT_EQ(b_.Screen().caller_identity, "account:carol");
  EXPECT_FALSE(b_.Screen().conflict);
  EXPECT_FALSE(b_.presenter->FailedPage().has_value());
}

TEST_F(CallFailedRedialTest, CallingFromAnotherChatOverTheKeptPageReplacesIt) {
  (void)FailedCall(a_, b_);
  (void)a_.presenter->Leave();
  PumpUntil([&]() { return b_.presenter->FailedPage().has_value(); });
  Thread carol;
  carol.id = "dm:account:carol";
  carol.kind = ThreadKind::Direct;
  carol.title = "Carol";
  carol.peer_identity_value = "account:carol";
  carol.updated_at = util::NowUnixMs();
  ASSERT_TRUE(b_.side.store->UpsertThread(carol));
  bool asked = false;
  CallPresenter::Callbacks callbacks;
  callbacks.confirm_end_current_call = [&asked](std::function<void(bool)> answer) {
    asked = true;
    answer(false);
  };
  b_.presenter->BindCallbacks(std::move(callbacks));
  ASSERT_TRUE(b_.presenter->CallFromChat(carol.id, false));
  PumpUntil([&]() { return !b_.presenter->ActiveCallId().empty(); });
  EXPECT_FALSE(asked) << "no call to end first";
  EXPECT_EQ(b_.Screen().kind, CallScreen::Kind::InCall);
  EXPECT_EQ(b_.Screen().thread_id, carol.id);
  EXPECT_FALSE(b_.presenter->FailedPage().has_value());
}

TEST_F(CallFailedRedialTest, ALockDropsTheKeptPageForGood) {
  (void)FailedCall(a_, b_);
  (void)b_.presenter->Leave();
  PumpUntil([&]() { return a_.presenter->FailedPage().has_value(); });
  ASSERT_TRUE(a_.presenter->FailedPage().has_value());
  a_unlocked_ = false;
  a_.presenter->Tick();
  EXPECT_FALSE(a_.presenter->FailedPage().has_value());
  a_unlocked_ = true;
  Pump();
  EXPECT_FALSE(a_.presenter->FailedPage().has_value()) << "not brought back";
}

TEST_F(CallFailedRedialTest, AnotherCallStackDropsTheKeptPage) {
  (void)FailedCall(a_, b_);
  (void)b_.presenter->Leave();
  PumpUntil([&]() { return a_.presenter->FailedPage().has_value(); });
  ASSERT_TRUE(a_.presenter->FailedPage().has_value());
  a_backend_ = b_.side.ui.get(); // another account's call stack
  a_.presenter->Tick();
  EXPECT_FALSE(a_.presenter->FailedPage().has_value());
  a_backend_ = nullptr;
  a_.presenter->Tick();
  EXPECT_FALSE(a_.presenter->FailedPage().has_value()) << "not brought back";
}

TEST_F(CallFailedRedialTest, ACallThatConnectedEndedByThePeerStillSaysSo) {
  const std::string call_id = Ring(a_, b_, false);
  ASSERT_EQ(b_.presenter->Accept(true, call_id), call_id);
  PumpUntil([&]() { return a_.side.ui->Phase() == CallPhase::InCall && b_.side.ui->Phase() == CallPhase::InCall; });
  PumpUntil([&]() { return b_.Screen().was_connected; });
  ASSERT_TRUE(b_.Screen().was_connected);
  (void)a_.presenter->Leave();
  PumpUntil([&]() { return b_.Screen().kind == CallScreen::Kind::None; });
  EXPECT_TRUE(b_.Told(CallNotice::Kind::PeerEnded));
  EXPECT_FALSE(b_.presenter->FailedPage().has_value());
}

TEST_F(CallFailedRedialTest, ADeclineStillSaysSo) {
  (void)Ring(a_, b_, false);
  (void)b_.presenter->Decline();
  PumpUntil([&]() { return a_.Screen().kind == CallScreen::Kind::None; });
  EXPECT_TRUE(a_.Told(CallNotice::Kind::PeerDeclined));
  EXPECT_FALSE(a_.presenter->FailedPage().has_value());
}

// Their Call again: the new invite overtakes the failed call's leave.
TEST_F(CallFailedRedialTest, TheFailedCallsPeerCallingAgainEndsItHereAndRingsWithNoConflict) {
  const std::string failed = FailedCall(a_, b_);
  b_.hold = IsCallEnd;
  a_.presenter->Redial();
  PumpUntil([&]() { return b_.Screen().kind == CallScreen::Kind::Ringing; });
  ASSERT_EQ(b_.Screen().kind, CallScreen::Kind::Ringing);
  EXPECT_NE(b_.Screen().ring_call_id, failed);
  EXPECT_FALSE(b_.Screen().conflict) << "no \"answering ends your other call\"";
  ASSERT_NE(Live(b_, failed), nullptr);
  EXPECT_EQ(Live(b_, failed)->State(), LiveCallState::Ended) << "the failed call ended here";
  ASSERT_FALSE(b_.held.empty()) << "the leave was held";
  b_.notices.clear();
  Release(b_);
  Pump();
  EXPECT_EQ(b_.Screen().kind, CallScreen::Kind::Ringing) << "the late leave changes nothing";
  EXPECT_FALSE(b_.presenter->FailedPage().has_value()) << "and does not bring the failed page back";
  EXPECT_FALSE(b_.Told(CallNotice::Kind::PeerEnded));
}

TEST_F(CallFailedRedialTest, TheFailedCallsOldInviteDeliveredAgainChangesNothing) {
  const std::string failed = FailedCall(a_, b_);
  for (const ThreadMessage& msg : sent_by_a_) {
    if (IsControl(msg, "call_invite") && msg.payload_json.find(failed) != std::string::npos) {
      (void)b_.side.Deliver(msg, a_.side.local_identity);
    }
  }
  Pump();
  EXPECT_TRUE(OffersRedial(b_.Screen())) << "still the failed page";
  EXPECT_EQ(Live(b_, failed)->State(), LiveCallState::Joined) << "not ended";
}

TEST_F(CallFailedRedialTest, TheSamePersonsGroupCallOverTheFailedCallRingsAsBefore) {
  const std::string failed = FailedCall(a_, b_);
  Thread group;
  group.id = "thread-group-1";
  group.kind = ThreadKind::Group;
  group.group_id = "group-1";
  group.title = "Group";
  group.updated_at = util::NowUnixMs();
  ASSERT_TRUE(b_.side.store->UpsertThread(group));
  ASSERT_TRUE(InviteFrom(b_, a_.side.local_identity, "call:group-1", std::string("group-1")));
  PumpUntil([&]() { return b_.Screen().kind == CallScreen::Kind::Ringing; });
  ASSERT_EQ(b_.Screen().kind, CallScreen::Kind::Ringing);
  EXPECT_TRUE(b_.Screen().conflict) << "answering ends the failed call: said, as before";
  EXPECT_NE(Live(b_, failed)->State(), LiveCallState::Ended) << "not ended here";
}

TEST_F(CallFailedRedialTest, SomeoneElseCallingOverTheFailedCallRingsAsBefore) {
  const std::string failed = FailedCall(a_, b_);
  ASSERT_TRUE(InviteFrom(b_, "account:carol", "call:carol-2"));
  PumpUntil([&]() { return b_.Screen().kind == CallScreen::Kind::Ringing; });
  ASSERT_EQ(b_.Screen().kind, CallScreen::Kind::Ringing);
  EXPECT_TRUE(b_.Screen().conflict);
  EXPECT_NE(Live(b_, failed)->State(), LiveCallState::Ended);
}

TEST_F(CallFailedRedialTest, TheSamePersonCallingOverACallStillConnectingRingsAsBefore) {
  const std::string call_id = AnsweredNotConnected(a_, b_);
  ASSERT_TRUE(InviteFrom(b_, a_.side.local_identity, "call:again-while-connecting"));
  PumpUntil([&]() { return b_.Screen().kind == CallScreen::Kind::Ringing; });
  ASSERT_EQ(b_.Screen().kind, CallScreen::Kind::Ringing);
  EXPECT_TRUE(b_.Screen().conflict);
  EXPECT_TRUE(b_.Screen().ending.same_peer);
  EXPECT_NE(Live(b_, call_id)->State(), LiveCallState::Ended) << "the call going on is not ended";
}

TEST_F(CallFailedRedialTest, BothPressingCallAgainEndInOneCall) {
  (void)FailedCall(a_, b_);
  a_.presenter->Redial();
  b_.presenter->Redial();
  EXPECT_TRUE(a_.presenter->RedialPending());
  EXPECT_TRUE(b_.presenter->RedialPending());
  PumpUntil([&]() {
    return a_.side.ui->Phase() == CallPhase::InCall && b_.side.ui->Phase() == CallPhase::InCall &&
           a_.side.ui->ActiveCallId() == b_.side.ui->ActiveCallId();
  });
  EXPECT_EQ(a_.side.ui->ActiveCallId(), b_.side.ui->ActiveCallId()) << "one call, both in it";
  EXPECT_EQ(a_.Screen().kind, CallScreen::Kind::InCall);
  EXPECT_EQ(b_.Screen().kind, CallScreen::Kind::InCall);
  EXPECT_EQ(a_.Screen().call_id, b_.Screen().call_id);
}

// The other phone's Try later reaches this one only after its Call again went out.
TEST_F(CallFailedRedialTest, ALateLeaveOfTheOldCallLeavesTheNewCallingPageAlone) {
  const std::string failed = FailedCall(a_, b_);
  a_.hold = IsCallEnd;
  (void)b_.presenter->Leave();
  Pump();
  a_.presenter->Redial();
  PumpUntil([&]() { return !a_.presenter->ActiveCallId().empty() && a_.presenter->ActiveCallId() != failed; });
  const std::string calling = a_.presenter->ActiveCallId();
  ASSERT_NE(calling, failed);
  a_.notices.clear();
  Release(a_);
  Pump();
  EXPECT_EQ(a_.presenter->ActiveCallId(), calling) << "the calling page as it was";
  EXPECT_EQ(a_.Screen().call_id, calling);
  EXPECT_FALSE(a_.presenter->FailedPage().has_value());
  EXPECT_TRUE(a_.notices.empty()) << "no pill";
}

TEST_F(CallFailedRedialTest, ARedialThatCannotStartIsNoLongerPending) {
  (void)FailedCall(a_, b_);
  ASSERT_TRUE(a_.side.store->DeleteThread(a_.thread_id));
  a_.presenter->Redial();
  Pump();
  EXPECT_FALSE(a_.presenter->RedialPending());
}

TEST_F(CallFailedRedialTest, APendingRedialIsForgottenAfterTenSeconds) {
  (void)FailedCall(a_, b_);
  a_.presenter->Redial(); // its start has not run yet (the owner's queue is not drained)
  ASSERT_TRUE(a_.presenter->RedialPending());
  a_.steady_ms += CallPresenter::kRedialPendingMs - 1;
  a_.presenter->Tick();
  EXPECT_TRUE(a_.presenter->RedialPending());
  a_.steady_ms += 1;
  a_.presenter->Tick();
  EXPECT_FALSE(a_.presenter->RedialPending()) << "ten seconds";
}

TEST_F(CallFailedRedialTest, APendingRedialIsForgottenOnALock) {
  (void)FailedCall(a_, b_);
  (void)b_.presenter->Leave();
  PumpUntil([&]() { return a_.presenter->FailedPage().has_value(); });
  ASSERT_TRUE(a_.presenter->FailedPage().has_value());
  a_.presenter->Redial();
  ASSERT_TRUE(a_.presenter->RedialPending());
  a_unlocked_ = false;
  a_.presenter->Tick();
  EXPECT_FALSE(a_.presenter->RedialPending()) << "a lock";
  a_unlocked_ = true;
}

TEST_F(CallFailedRedialTest, APendingRedialIsForgottenOnAnotherAccount) {
  (void)FailedCall(a_, b_);
  (void)b_.presenter->Leave();
  PumpUntil([&]() { return a_.presenter->FailedPage().has_value(); });
  a_.presenter->Redial();
  ASSERT_TRUE(a_.presenter->RedialPending());
  a_backend_ = b_.side.ui.get();
  a_.presenter->Tick();
  EXPECT_FALSE(a_.presenter->RedialPending()) << "another account's call stack";
  a_backend_ = nullptr;
}

TEST_F(CallFailedRedialTest, AfterAPendingRedialClearsCallAgainWorksAgain) {
  const std::string first = FailedCall(a_, b_);
  a_.presenter->Redial();
  a_.steady_ms += CallPresenter::kRedialPendingMs;
  a_.presenter->Tick(); // forgotten before its call showed
  ASSERT_FALSE(a_.presenter->RedialPending());
  a_.presenter->Leave(); // nothing on the new call yet; end whatever started
  Pump();
  const std::string second = FailedCall(a_, b_);
  ASSERT_NE(second, first);
  const size_t sent = sent_by_a_.size();
  a_.presenter->Redial();
  PumpUntil([&]() { return !a_.presenter->ActiveCallId().empty() && a_.presenter->ActiveCallId() != second; });
  EXPECT_EQ(InvitesSent(sent_by_a_, sent).size(), 1u);
}

} // namespace
} // namespace pbr
