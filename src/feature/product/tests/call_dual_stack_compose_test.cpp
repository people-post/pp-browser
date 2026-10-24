#include "feature/product/tests/call_stack_compose_support.h"
#include "lib/calls/CallControlCodec.h"
#include "common/ValueJson.h"
#include "common/MeshAlias.h"

#include <deque>
#include <tuple>
#include <string>

namespace pbr {
namespace {

using test::BuildStackSide;
using test::DestroyStackSide;
using test::DrainUntil;
using test::SoftStopStackSide;
using test::StackSide;
using test::StartCallNow;

class CallDualStackComposeTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    AppRuntime::Initialize(ManualOwnerRuntimeConfig());
    AppRuntime::InitializeUI();

    offer_.on_send = [this](const std::string&, const ThreadMessage& msg) { answer_inbox_.push_back(msg); };
    answer_.on_send = [this](const std::string&, const ThreadMessage& msg) { offer_inbox_.push_back(msg); };
    BuildStackSide(offer_, "offer", 0xa0);
    BuildStackSide(answer_, "answer", 0xb0);

    // Each side treats the other as already connected for EnsurePeerReachable.
    offer_.dial->connected[answer_.local_identity] = true;
    offer_.dial->endpoints[answer_.local_identity] =
        "/ip4/127.0.0.1/udp/47101/adp/1.0.0/p2p/12D3KooWAnswer";
    answer_.dial->connected[offer_.local_identity] = true;
    answer_.dial->endpoints[offer_.local_identity] =
        "/ip4/127.0.0.1/udp/47100/adp/1.0.0/p2p/12D3KooWOffer";

    // Answerer reverse-dial arms offerer inbound (product: stream lands on offerer before dial).
    answer_.transport->peer_inbound = [this](CallMediaDirectConnectParams params) {
      if (!offer_.transport || !offer_.transport->inbound.Installed()) {
        return;
      }
      // As on the wire: the offerer's inbound hello names the dialer (the answerer), not itself.
      params.peer_key = answer_.local_identity;
      // Leg coordinator marks the accepting transport active before invoking the handler.
      offer_.transport->active = true;
      offer_.transport->active_params = params;
      offer_.transport->inbound.DeliverThen(std::move(params),
                                            [](CallMediaDirectConnectParams, CallMediaDirectCallbacks cbs) {
                                              if (cbs.on_connected) {
                                                cbs.on_connected();
                                              }
                                            });
    };
  }

  void TearDown() override {
    // Soft-stop stacks first, then join AppRuntime before destroying stores — DrainWorkersThenUI
    // alone does not wait for every pool thread (PR #216 follow-up).
    SoftStopStackSide(offer_);
    SoftStopStackSide(answer_);
    AppRuntime::ShutdownUI();
    AppRuntime::Shutdown();
    DestroyStackSide(offer_);
    DestroyStackSide(answer_);
  }

  static const LiveCall* Live(StackSide& side, const std::string& call_id) {
    return side.stack->Calls() ? side.stack->Calls()->Live().Find(call_id) : nullptr;
  }
  static void ExpectLive(StackSide& side, const std::string& call_id, const LiveCallState state,
                         const LiveCallEndReason reason, const char* who) {
    const LiveCall* call = Live(side, call_id);
    ASSERT_NE(call, nullptr) << who;
    EXPECT_EQ(call->State(), state) << who << " state=" << LiveCallStateName(call->State());
    EXPECT_EQ(call->EndReason(), reason) << who << " reason=" << LiveCallEndReasonName(call->EndReason());
  }

  /** Deliver queued call-control between the two stacks; drain UI between hops. */
  void PumpWire(int rounds = 8) {
    for (int r = 0; r < rounds; ++r) {
      bool moved = false;
      while (!answer_inbox_.empty()) {
        ThreadMessage msg = answer_inbox_.front();
        answer_inbox_.pop_front();
        ASSERT_TRUE(answer_.Deliver(msg, offer_.local_identity));
        moved = true;
      }
      while (!offer_inbox_.empty()) {
        ThreadMessage msg = offer_inbox_.front();
        offer_inbox_.pop_front();
        if (hold_accepts_to_offer_ && msg.payload_json.find("call_accept") != std::string::npos) {
          held_to_offer_.push_back(std::move(msg));  // a relay that has not delivered it yet
          continue;
        }
        ASSERT_TRUE(offer_.Deliver(msg, answer_.local_identity));
        moved = true;
      }
      AppRuntime::RunUIAndOwnerTasks();
      if (!moved) {
        break;
      }
    }
  }

  /** Offer StartCall → Answer Accept → both InCall. Leaves call active. */
  std::string RunOfferAnswerToInCall(const std::string& thread_id) {
    auto started = StartCallNow(*offer_.ui, thread_id, false, {answer_.local_identity});
    EXPECT_TRUE(started) << (started ? "" : started.error().message);
    if (!started) {
      return {};
    }
    const std::string call_id = started->call_id;

    auto key = offer_.stack->MediaKeys()->LoadEpochKey(call_id, 1);
    EXPECT_TRUE(key && key->has_value());
    if (!key || !key->has_value()) {
      return {};
    }
    EXPECT_TRUE(answer_.stack->MediaKeys()->PutEpochKey(call_id, 1, **key));

    PumpWire();
    auto pending = answer_.ui->TopPendingInvite();
    EXPECT_TRUE(pending && pending->has_value()) << "invite should land on answerer";
    if (!pending || !pending->has_value()) {
      return {};
    }

    answer_.ui->Apply(CallLifecycleEvent::InviteSeen, call_id);
    answer_.ui->Apply(CallLifecycleEvent::AcceptClicked, call_id);

    DrainUntil([&]() {
      PumpWire();
      const bool answer_ok =
          answer_.stack->MediaEngine() && answer_.stack->MediaEngine()->IsActive() &&
          answer_.ui->Phase() == CallPhase::InCall;
      const bool offer_ok = offer_.stack->MediaEngine() && offer_.stack->MediaEngine()->IsActive() &&
                            offer_.ui->Phase() == CallPhase::InCall;
      return answer_ok && offer_ok;
    });
    EXPECT_EQ(answer_.ui->Phase(), CallPhase::InCall);
    EXPECT_EQ(offer_.ui->Phase(), CallPhase::InCall);
    return call_id;
  }

  void FinishAnswerLeaveExpectBothIdle(const std::string& call_id) {
    answer_.ui->Apply(CallLifecycleEvent::LeaveClicked, call_id);
    // Product: answer Leave → CallLeave/CallEnded on wire → offer EndCallLocal → RemoteEnded.
    // Do not chrome-heal with offer LeaveClicked — that masked the missing RemoteEnded wire.
    DrainUntil([&]() {
      PumpWire();
      const bool answer_idle = answer_.ui->Phase() == CallPhase::Idle && !answer_.stack->HasActiveLocalCall();
      const bool offer_idle = offer_.ui->Phase() == CallPhase::Idle && !offer_.stack->HasActiveLocalCall();
      return answer_idle && offer_idle;
    });
    EXPECT_EQ(answer_.ui->Phase(), CallPhase::Idle);
    EXPECT_EQ(offer_.ui->Phase(), CallPhase::Idle)
        << "offerer must Idle on remote Leave without local LeaveClicked";
    EXPECT_FALSE(answer_.stack->HasActiveLocalCall());
    EXPECT_FALSE(offer_.stack->HasActiveLocalCall());
    offer_inbox_.clear();
    answer_inbox_.clear();
  }

  void FinishOfferLeaveExpectBothIdle(const std::string& call_id) {
    offer_.ui->Apply(CallLifecycleEvent::LeaveClicked, call_id);
    // Symmetric: offerer Leave must Idle answerer without answer LeaveClicked.
    DrainUntil([&]() {
      PumpWire();
      const bool answer_idle = answer_.ui->Phase() == CallPhase::Idle && !answer_.stack->HasActiveLocalCall();
      const bool offer_idle = offer_.ui->Phase() == CallPhase::Idle && !offer_.stack->HasActiveLocalCall();
      return answer_idle && offer_idle;
    });
    EXPECT_EQ(offer_.ui->Phase(), CallPhase::Idle);
    EXPECT_EQ(answer_.ui->Phase(), CallPhase::Idle)
        << "answerer must Idle on remote Leave without local LeaveClicked";
    EXPECT_FALSE(answer_.stack->HasActiveLocalCall());
    EXPECT_FALSE(offer_.stack->HasActiveLocalCall());
    offer_inbox_.clear();
    answer_inbox_.clear();
  }

  /** Offer StartCall → Answer Accept → both InCall → Answer Leave → both Idle (no offer heal). */
  std::string RunOfferAnswerInCallLeave(const std::string& thread_id) {
    const std::string call_id = RunOfferAnswerToInCall(thread_id);
    if (call_id.empty()) {
      return {};
    }
    FinishAnswerLeaveExpectBothIdle(call_id);
    return call_id;
  }

  StackSide offer_;
  StackSide answer_;
  std::deque<ThreadMessage> offer_inbox_;
  std::deque<ThreadMessage> answer_inbox_;
  /** B30: hold CallAccepts on their way to the offerer (late relay). */
  bool hold_accepts_to_offer_ = false;
  std::deque<ThreadMessage> held_to_offer_;
};

TEST_F(CallDualStackComposeTest, OfferInviteAcceptInCallLeave) {
  // Product-shaped wire: Offer StartCall → Invite → Answer Accept → CallAccept → both media → Leave.
  Thread thread;
  // Windows: thread id is a directory name under threads/ — no ':' (illegal path char).
  thread.id = "thread-dual-dm";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));

  const std::string call_id = RunOfferAnswerInCallLeave(thread.id);
  ASSERT_FALSE(call_id.empty());
  EXPECT_GE(answer_.transport->connect_async_calls, 1);
  // Each side's LiveCall ended for its own reason: the answerer left, the offerer was left.
  ExpectLive(answer_, call_id, LiveCallState::Ended, LiveCallEndReason::LocalLeave, "answer");
  ExpectLive(offer_, call_id, LiveCallState::Ended, LiveCallEndReason::RemoteEnded, "offer");
}

/** The summary a side wrote, decoded (control type checked). */
std::optional<CallSummaryDetail> SummaryOf(const ThreadMessage& message) {
  auto payload = TryParseObject(message.payload_json);
  if (!payload || payload->getString("control_type") != std::optional<std::string>("call_summary")) {
    return std::nullopt;
  }
  auto detail = payload->getString("detail");
  if (!detail) {
    return std::nullopt;
  }
  auto decoded = CallControlCodec::DecodeSummary(*detail);
  return decoded ? std::optional<CallSummaryDetail>(*decoded) : std::nullopt;
}

TEST_F(CallDualStackComposeTest, TheStarterWritesOneSummaryIntoItsThread) {
  Thread thread;
  thread.id = "thread-dual-summary";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));

  const std::string call_id = RunOfferAnswerInCallLeave(thread.id);
  ASSERT_FALSE(call_id.empty());
  ASSERT_EQ(offer_.summaries.size(), 1u) << "one line per call, by its starter";
  EXPECT_EQ(offer_.summaries.front().thread_id, thread.id);
  const auto summary = SummaryOf(offer_.summaries.front());
  ASSERT_TRUE(summary);
  EXPECT_EQ(summary->call_id, call_id);
  EXPECT_EQ(summary->outcome, CallOutcome::Completed);
  EXPECT_TRUE(summary->duration_ms.has_value());
  EXPECT_FALSE(summary->group);
  EXPECT_TRUE(answer_.summaries.empty()) << "the invitee reads the starter's summary";
}

TEST_F(CallDualStackComposeTest, TheCalleeFilesTheCallUnderItsOwnDm) {
  Thread thread;
  thread.id = "thread-dual-callee";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));
  auto started = StartCallNow(*offer_.ui, thread.id, false, {answer_.local_identity});
  ASSERT_TRUE(started) << started.error().message;
  PumpWire();
  auto pending = answer_.ui->TopPendingInvite();
  ASSERT_TRUE(pending && pending->has_value());
  EXPECT_EQ((*pending)->origin_thread_id, std::optional<std::string>("dm:" + offer_.local_identity))
      << "not the caller's thread id: that one means nothing here";
}

TEST_F(CallDualStackComposeTest, HangingUpBeforeAnAnswerIsSummarizedAsCancelled) {
  Thread thread;
  thread.id = "thread-dual-cancel";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));
  auto started = StartCallNow(*offer_.ui, thread.id, false, {answer_.local_identity});
  ASSERT_TRUE(started) << started.error().message;
  PumpWire();
  offer_.ui->Apply(CallLifecycleEvent::LeaveClicked, started->call_id);
  DrainUntil([&]() {
    PumpWire();
    return !offer_.stack->HasActiveLocalCall();
  });
  ASSERT_EQ(offer_.summaries.size(), 1u);
  const auto summary = SummaryOf(offer_.summaries.front());
  ASSERT_TRUE(summary);
  EXPECT_EQ(summary->outcome, CallOutcome::Cancelled) << "rang, nobody answered: the callee missed it";
  EXPECT_FALSE(summary->duration_ms.has_value()) << "ringing is not talk time";
}

/** A direct thread on `side` for the other side's account. */
Thread DmOn(StackSide& side, const std::string& id) {
  Thread thread;
  thread.id = id;
  thread.kind = ThreadKind::Direct;
  thread.title = "Peer";
  thread.updated_at = util::NowUnixMs();
  EXPECT_TRUE(side.store->UpsertThread(thread));
  return thread;
}

TEST_F(CallDualStackComposeTest, ARedialStormEndsInOneCallThatConnects) {
  // Couldn't get through, so: call, hang up, call again — five times in a few seconds.
  const Thread thread = DmOn(offer_, "thread-dual-redial");
  std::string last_call;
  for (int attempt = 0; attempt < 5; ++attempt) {
    auto started = StartCallNow(*offer_.ui, thread.id, false, {answer_.local_identity});
    ASSERT_TRUE(started) << "attempt " << attempt << ": " << started.error().message;
    last_call = started->call_id;
    auto key = offer_.stack->MediaKeys()->LoadEpochKey(last_call, 1);
    ASSERT_TRUE(key && key->has_value());
    ASSERT_TRUE(answer_.stack->MediaKeys()->PutEpochKey(last_call, 1, **key));
    PumpWire(2);
    if (attempt < 4) {
      offer_.ui->Apply(CallLifecycleEvent::LeaveClicked, last_call);
      PumpWire(2);
    }
  }
  PumpWire();
  auto pending = answer_.ui->TopPendingInvite();
  ASSERT_TRUE(pending && pending->has_value()) << "the last call rings";
  EXPECT_EQ((*pending)->call_id, last_call) << "not a stale one from the storm";
  answer_.ui->Apply(CallLifecycleEvent::InviteSeen, last_call);
  answer_.ui->Apply(CallLifecycleEvent::AcceptClicked, last_call);
  DrainUntil([&]() {
    PumpWire();
    return answer_.ui->Phase() == CallPhase::InCall && offer_.ui->Phase() == CallPhase::InCall;
  });
  EXPECT_EQ(answer_.ui->Phase(), CallPhase::InCall);
  EXPECT_EQ(offer_.ui->Phase(), CallPhase::InCall);
  FinishAnswerLeaveExpectBothIdle(last_call);
  pending = answer_.ui->TopPendingInvite();
  EXPECT_TRUE(!pending || !pending->has_value()) << "nothing left ringing after the storm";
}

TEST_F(CallDualStackComposeTest, CallingEachOtherAtOnceMergesIntoOneCall) {
  // Both press Call before either invite lands. Each side keeps the lower call id: the side whose
  // call lost answers the winner and drops its own. No ring, no tap — one call.
  const Thread offer_thread = DmOn(offer_, "thread-dual-glare-a");
  const Thread answer_thread = DmOn(answer_, "thread-dual-glare-b");
  auto from_offer = StartCallNow(*offer_.ui, offer_thread.id, false, {answer_.local_identity});
  auto from_answer = StartCallNow(*answer_.ui, answer_thread.id, false, {offer_.local_identity});
  ASSERT_TRUE(from_offer) << from_offer.error().message;
  ASSERT_TRUE(from_answer) << from_answer.error().message;
  for (const auto& [call, from, to] : {std::tuple{from_offer->call_id, &offer_, &answer_},
                                       std::tuple{from_answer->call_id, &answer_, &offer_}}) {
    auto key = from->stack->MediaKeys()->LoadEpochKey(call, 1);
    ASSERT_TRUE(key && key->has_value());
    ASSERT_TRUE(to->stack->MediaKeys()->PutEpochKey(call, 1, **key));
  }
  const std::string winner = std::min(from_offer->call_id, from_answer->call_id);
  DrainUntil([&]() {
    PumpWire();
    return answer_.ui->Phase() == CallPhase::InCall && offer_.ui->Phase() == CallPhase::InCall;
  });
  EXPECT_EQ(offer_.ui->Phase(), CallPhase::InCall) << "offer side";
  EXPECT_EQ(answer_.ui->Phase(), CallPhase::InCall) << "answer side";
  EXPECT_EQ(offer_.ui->ActiveCallId(), winner);
  EXPECT_EQ(answer_.ui->ActiveCallId(), winner);
  for (StackSide* side : {&offer_, &answer_}) {
    auto pending = side->ui->TopPendingInvite();
    EXPECT_TRUE(pending && !pending->has_value()) << "nothing rings";
  }
}

TEST_F(CallDualStackComposeTest, CallingBackWhileTheirCallRingsAnswersIt) {
  const Thread offer_thread = DmOn(offer_, "thread-dual-callback-a");
  const Thread answer_thread = DmOn(answer_, "thread-dual-callback-b");
  auto placed = StartCallNow(*offer_.ui, offer_thread.id, false, {answer_.local_identity});
  ASSERT_TRUE(placed) << placed.error().message;
  auto key = offer_.stack->MediaKeys()->LoadEpochKey(placed->call_id, 1);
  ASSERT_TRUE(key && key->has_value());
  ASSERT_TRUE(answer_.stack->MediaKeys()->PutEpochKey(placed->call_id, 1, **key));
  DrainUntil([&]() {
    PumpWire();
    auto pending = answer_.ui->TopPendingInvite();
    return pending && pending->has_value();
  });
  auto called_back = StartCallNow(*answer_.ui, answer_thread.id, false, {offer_.local_identity});
  ASSERT_TRUE(called_back) << called_back.error().message;
  EXPECT_EQ(called_back->call_id, placed->call_id) << "calling back is answering";
  DrainUntil([&]() {
    PumpWire();
    return answer_.ui->Phase() == CallPhase::InCall && offer_.ui->Phase() == CallPhase::InCall;
  });
  EXPECT_EQ(offer_.ui->ActiveCallId(), placed->call_id);
  EXPECT_EQ(answer_.ui->ActiveCallId(), placed->call_id);
}

// H012 / H011 over call-control: a signaled punch runs offer → answer → both bursts, and our R1
// reaches the peer's late reserve.
TEST_F(CallDualStackComposeTest, BuildingSessionsAgainKeepsACallAlreadyRinging) {
  // The invite reaches the answerer over the relay before its mesh is up; when the mesh came up the
  // hub built the call sessions again, which dropped the ringing call (state and all).
  Thread thread;
  thread.id = "thread-dual-rebuild";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));
  auto started = StartCallNow(*offer_.ui, thread.id, false, {answer_.local_identity});
  ASSERT_TRUE(started) << started.error().message;
  const std::string call_id = started->call_id;
  auto key = offer_.stack->MediaKeys()->LoadEpochKey(call_id, 1);
  ASSERT_TRUE(key && key->has_value());
  ASSERT_TRUE(answer_.stack->MediaKeys()->PutEpochKey(call_id, 1, **key));
  PumpWire();
  auto pending = answer_.ui->TopPendingInvite();
  ASSERT_TRUE(pending && pending->has_value()) << "invite should land on answerer";

  answer_.stack->BuildSessions(answer_.deps);
  AppRuntime::RunUIAndOwnerTasks();

  pending = answer_.ui->TopPendingInvite();
  ASSERT_TRUE(pending && pending->has_value()) << "the ringing call survives";
  EXPECT_EQ((*pending)->call_id, call_id);

  answer_.ui->Apply(CallLifecycleEvent::InviteSeen, call_id);
  answer_.ui->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  DrainUntil([&]() {
    PumpWire();
    return answer_.ui->Phase() == CallPhase::InCall && offer_.ui->Phase() == CallPhase::InCall;
  });
  EXPECT_EQ(answer_.ui->Phase(), CallPhase::InCall);
  EXPECT_EQ(offer_.ui->Phase(), CallPhase::InCall);
  FinishAnswerLeaveExpectBothIdle(call_id);
}

TEST_F(CallDualStackComposeTest, ReachSignalsTravelOverCallControl) {
  Thread thread;
  thread.id = "thread-dual-reach";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));
  const std::string call_id = RunOfferAnswerToInCall(thread.id);
  ASSERT_FALSE(call_id.empty());

  struct MeshFake {
    std::vector<std::string> candidates;
    std::vector<std::vector<std::string>> bursts;
    std::vector<std::string> preferred;
  };
  MeshFake offer_mesh{{"/ip4/1.1.1.1/udp/1"}, {}, {}};
  MeshFake answer_mesh{{"/ip4/2.2.2.2/udp/2"}, {}, {}};
  for (auto [side, fake] : {std::pair{&offer_, &offer_mesh}, std::pair{&answer_, &answer_mesh}}) {
    CallReachSignals::MeshPorts ports;
    ports.local_punch_addrs = [fake]() { return fake->candidates; };
    ports.punch_burst = [fake](const std::vector<std::string>& addrs, int, mesh::SignalingPunchExchange::DoneFn done) {
      fake->bursts.push_back(addrs);
      done(Roe<void>{});
    };
    ports.prefer_late_reserve = [fake](const std::string& r1) { fake->preferred.push_back(r1); };
    side->stack->Calls()->ReachSignals().SetMeshPorts(std::move(ports));
  }

  std::optional<bool> punched;
  offer_.stack->Calls()->ReachSignals().RequestSignalingPunch("12D3KooWAnswer", offer_mesh.candidates,
                                                              [&](Roe<void> r) { punched = static_cast<bool>(r); });
  PumpWire();
  ASSERT_EQ(answer_mesh.bursts.size(), 1u) << "the answer side bursts at the offer's candidates";
  EXPECT_EQ(answer_mesh.bursts[0], offer_mesh.candidates);
  ASSERT_EQ(offer_mesh.bursts.size(), 1u) << "the offer side bursts at the answer's candidates";
  EXPECT_EQ(offer_mesh.bursts[0], answer_mesh.candidates);
  ASSERT_TRUE(punched);
  EXPECT_TRUE(*punched);

  offer_.stack->Calls()->ReachSignals().AnnounceCircuitR1("12D3KooWR1");
  PumpWire();
  EXPECT_EQ(answer_mesh.preferred, std::vector<std::string>{"12D3KooWR1"});
  FinishAnswerLeaveExpectBothIdle(call_id);
}

// B30 (call-path-resilience k4): the relay delivers CallAccept late (CN cellular: 11–58 s) while the
// answerer's call-media hello — keyed from the invite — reaches the offerer directly. The hello
// stands in for the accept; the real one arriving later changes nothing.
TEST_F(CallDualStackComposeTest, AnswerersHelloActsAsAcceptWhenTheRelayAcceptIsLate) {
  Thread thread;
  thread.id = "thread-dual-late-accept";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));

  auto started = StartCallNow(*offer_.ui, thread.id, false, {answer_.local_identity});
  ASSERT_TRUE(started) << started.error().message;
  const std::string call_id = started->call_id;
  auto key = offer_.stack->MediaKeys()->LoadEpochKey(call_id, 1);
  ASSERT_TRUE(key && key->has_value());
  ASSERT_TRUE(answer_.stack->MediaKeys()->PutEpochKey(call_id, 1, **key));
  PumpWire();
  ASSERT_TRUE(answer_.ui->TopPendingInvite() && answer_.ui->TopPendingInvite()->has_value());

  hold_accepts_to_offer_ = true;
  answer_.ui->Apply(CallLifecycleEvent::InviteSeen, call_id);
  answer_.ui->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  DrainUntil([&]() {
    PumpWire();
    return offer_.stack->MediaEngine() && offer_.stack->MediaEngine()->IsActive() &&
           offer_.ui->Phase() == CallPhase::InCall && answer_.ui->Phase() == CallPhase::InCall;
  });
  ASSERT_FALSE(held_to_offer_.empty()) << "the accept never reached the offerer";
  EXPECT_EQ(offer_.ui->Phase(), CallPhase::InCall) << "the answerer's hello stood in for the accept";
  EXPECT_TRUE(offer_.stack->MediaEngine()->IsActive());
  // The answer mode (e.g. voice-only) rides the real accept: until it lands the offerer holds its
  // auto camera (PR #230 review).
  auto awaiting = offer_.stack->Calls()->AwaitingExplicitAnswerForCall(call_id);
  ASSERT_TRUE(awaiting);
  EXPECT_TRUE(*awaiting) << "an implicit accept is not the answer yet";

  // The relay finally delivers the accept: idempotent.
  hold_accepts_to_offer_ = false;
  for (auto& msg : held_to_offer_) {
    offer_inbox_.push_back(std::move(msg));
  }
  held_to_offer_.clear();
  PumpWire();
  EXPECT_EQ(offer_.ui->Phase(), CallPhase::InCall);
  EXPECT_TRUE(offer_.stack->MediaEngine()->IsActive());
  awaiting = offer_.stack->Calls()->AwaitingExplicitAnswerForCall(call_id);
  ASSERT_TRUE(awaiting);
  EXPECT_FALSE(*awaiting) << "the late CallAccept is the answer";
  FinishAnswerLeaveExpectBothIdle(call_id);
}

// Failed is not closed: both sides' connects fail and both show ConnectFailed, the call still open.
// The answerer's Retry dials the offerer, whose failed-but-open call accepts that connection and
// resumes media over it — both sides InCall, with no redial on the offerer.
TEST_F(CallDualStackComposeTest, RetryFromOneSideReconnectsAFailedOpenCallOnBoth) {
  Thread thread;
  thread.id = "thread-dual-fail-retry";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));
  offer_.transport->fail_connects = true;
  answer_.transport->fail_connects = true;
  auto started = StartCallNow(*offer_.ui, thread.id, false, {answer_.local_identity});
  ASSERT_TRUE(started) << started.error().message;
  const std::string call_id = started->call_id;
  auto key = offer_.stack->MediaKeys()->LoadEpochKey(call_id, 1);
  ASSERT_TRUE(key && key->has_value());
  ASSERT_TRUE(answer_.stack->MediaKeys()->PutEpochKey(call_id, 1, **key));
  PumpWire();
  answer_.ui->Apply(CallLifecycleEvent::InviteSeen, call_id);
  answer_.ui->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  DrainUntil(
      [&]() {
        PumpWire();
        return offer_.ui->Phase() == CallPhase::ConnectFailed && answer_.ui->Phase() == CallPhase::ConnectFailed;
      },
      30000);
  ASSERT_EQ(offer_.ui->Phase(), CallPhase::ConnectFailed);
  ASSERT_EQ(answer_.ui->Phase(), CallPhase::ConnectFailed);
  // Failed is not closed: both LiveCalls are still Joined.
  ExpectLive(offer_, call_id, LiveCallState::Joined, LiveCallEndReason::None, "offer failed");
  ExpectLive(answer_, call_id, LiveCallState::Joined, LiveCallEndReason::None, "answer failed");
  const int offer_dials = offer_.transport->connect_async_calls;

  answer_.transport->fail_connects = false;
  answer_.ui->Apply(CallLifecycleEvent::RetryClicked, call_id);
  DrainUntil([&]() {
    PumpWire();
    return offer_.ui->Phase() == CallPhase::InCall && answer_.ui->Phase() == CallPhase::InCall;
  });
  EXPECT_EQ(answer_.ui->Phase(), CallPhase::InCall);
  EXPECT_EQ(offer_.ui->Phase(), CallPhase::InCall) << "the offerer's failed call accepted the peer's retry";
  EXPECT_TRUE(offer_.stack->MediaEngine() && offer_.stack->MediaEngine()->IsActive());
  EXPECT_EQ(offer_.transport->connect_async_calls, offer_dials) << "resumed over the inbound stream, no redial";
  FinishAnswerLeaveExpectBothIdle(call_id);
  const auto offer_end = offer_.ui->TakeRemoteEnd();
  ASSERT_TRUE(offer_end.has_value());
  EXPECT_EQ(offer_end->call_id, call_id);
  EXPECT_FALSE(offer_end->declined);
}

TEST_F(CallDualStackComposeTest, OfferLeaveClearsAnswererIdle) {
  // CALLS remote end (symmetric): offerer Leave → answerer Idle without answer LeaveClicked.
  Thread thread;
  thread.id = "thread-dual-offer-leave";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));

  const std::string call_id = RunOfferAnswerToInCall(thread.id);
  ASSERT_FALSE(call_id.empty());
  FinishOfferLeaveExpectBothIdle(call_id);
  // The answerer is told why its call vanished — once; the side that left is not.
  const auto answer_end = answer_.ui->TakeRemoteEnd();
  ASSERT_TRUE(answer_end.has_value());
  EXPECT_EQ(answer_end->call_id, call_id);
  EXPECT_FALSE(answer_.ui->TakeRemoteEnd().has_value()) << "once per call";
  EXPECT_FALSE(offer_.ui->TakeRemoteEnd().has_value()) << "the leaver ended it itself";
}

TEST_F(CallDualStackComposeTest, OfferAnswerKCycleTeardown) {
  // B-TEARDOWN: Leave→Idle→second Invite→InCall on dual CallStack (no stuck listen/media).
  Thread thread;
  thread.id = "thread-dual-kcycle";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));

  const int connect_before = answer_.transport->connect_async_calls;
  const std::string call_1 = RunOfferAnswerInCallLeave(thread.id);
  ASSERT_FALSE(call_1.empty());
  EXPECT_FALSE(offer_.stack->WantEphemeralListen());
  EXPECT_FALSE(answer_.stack->WantEphemeralListen());
  EXPECT_EQ(offer_.ui->Phase(), CallPhase::Idle);
  EXPECT_EQ(answer_.ui->Phase(), CallPhase::Idle);

  const std::string call_2 = RunOfferAnswerInCallLeave(thread.id);
  ASSERT_FALSE(call_2.empty());
  EXPECT_NE(call_1, call_2);
  EXPECT_GT(answer_.transport->connect_async_calls, connect_before);
  EXPECT_EQ(offer_.ui->Phase(), CallPhase::Idle);
  EXPECT_EQ(answer_.ui->Phase(), CallPhase::Idle);
  EXPECT_FALSE(offer_.stack->HasActiveLocalCall());
  EXPECT_FALSE(answer_.stack->HasActiveLocalCall());
}

TEST_F(CallDualStackComposeTest, OfferInviteAnswerDeclineClearsOfferer) {
  // CALLS: answer Decline → CallDecline on wire → offerer Idle (no LeaveClicked / TTL).
  Thread thread;
  thread.id = "thread-dual-decline";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));

  auto started = StartCallNow(*offer_.ui, thread.id, false, {answer_.local_identity});
  ASSERT_TRUE(started) << started.error().message;
  const std::string call_id = started->call_id;
  EXPECT_EQ(offer_.ui->Phase(), CallPhase::OutboundCalling);

  PumpWire();
  auto pending = answer_.ui->TopPendingInvite();
  ASSERT_TRUE(pending && pending->has_value());
  EXPECT_EQ((*pending)->call_id, call_id);

  answer_.ui->Apply(CallLifecycleEvent::InviteSeen, call_id);
  answer_.ui->Apply(CallLifecycleEvent::DeclineClicked, call_id);

  DrainUntil([&]() {
    PumpWire();
    return answer_.ui->Phase() == CallPhase::Idle && offer_.ui->Phase() == CallPhase::Idle &&
           !offer_.stack->HasActiveLocalCall();
  });
  EXPECT_EQ(answer_.ui->Phase(), CallPhase::Idle);
  EXPECT_EQ(offer_.ui->Phase(), CallPhase::Idle)
      << "offerer must Idle on remote Decline without local LeaveClicked";
  EXPECT_FALSE(offer_.stack->HasActiveLocalCall());
  EXPECT_FALSE(answer_.stack->HasActiveLocalCall());
  ExpectLive(answer_, call_id, LiveCallState::Ended, LiveCallEndReason::Declined, "answer");
  ExpectLive(offer_, call_id, LiveCallState::Ended, LiveCallEndReason::DeclinedByPeer, "offer");
  // The caller is told the call was declined; the one who declined is told nothing.
  const auto offer_end = offer_.ui->TakeRemoteEnd();
  ASSERT_TRUE(offer_end.has_value());
  EXPECT_TRUE(offer_end->declined);
  EXPECT_FALSE(answer_.ui->TakeRemoteEnd().has_value());
}

TEST_F(CallDualStackComposeTest, AcceptSecondInviteEndsPriorActiveCall) {
  // B-CONFLICT: Accept call B while InCall on A ends A (LeaveCallIfActiveExcept) across stacks.
  Thread thread;
  thread.id = "thread-dual-conflict";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));

  const std::string call_a = RunOfferAnswerToInCall(thread.id);
  ASSERT_FALSE(call_a.empty());
  auto active_a = answer_.ui->ActiveLocalCall();
  ASSERT_TRUE(active_a && active_a->has_value());
  EXPECT_EQ((*active_a)->call_id, call_a);

  // Second outbound invite while still InCall on A.
  auto started_b = StartCallNow(*offer_.ui, thread.id, false, {answer_.local_identity});
  ASSERT_TRUE(started_b) << started_b.error().message;
  const std::string call_b = started_b->call_id;
  EXPECT_NE(call_a, call_b);
  // Offerer LeaveCallIfActiveExcept ends A when starting B.
  {
    auto offer_active = offer_.ui->ActiveLocalCall();
    ASSERT_TRUE(offer_active && offer_active->has_value());
    EXPECT_EQ((*offer_active)->call_id, call_b);
  }

  auto key_b = offer_.stack->MediaKeys()->LoadEpochKey(call_b, 1);
  ASSERT_TRUE(key_b && key_b->has_value());
  ASSERT_TRUE(answer_.stack->MediaKeys()->PutEpochKey(call_b, 1, **key_b));

  PumpWire();
  // Answer may still show A active until Accept B; pending B should be visible.
  auto pending_b = answer_.ui->TopPendingInvite();
  ASSERT_TRUE(pending_b && pending_b->has_value());
  EXPECT_EQ((*pending_b)->call_id, call_b);

  answer_.ui->Apply(CallLifecycleEvent::InviteSeen, call_b);
  answer_.ui->Apply(CallLifecycleEvent::AcceptClicked, call_b);

  DrainUntil([&]() {
    PumpWire();
    auto active = answer_.ui->ActiveLocalCall();
    return active && active->has_value() && (*active)->call_id == call_b &&
           answer_.ui->Phase() == CallPhase::InCall && offer_.ui->Phase() == CallPhase::InCall;
  });
  auto active_b = answer_.ui->ActiveLocalCall();
  ASSERT_TRUE(active_b && active_b->has_value());
  EXPECT_EQ((*active_b)->call_id, call_b);

  // Prior call A must be ended on both sides.
  auto offer_a = offer_.stack->Calls()->ActiveLocalCall();
  ASSERT_TRUE(offer_a);
  if (offer_a->has_value()) {
    EXPECT_NE((*offer_a)->call_id, call_a);
  }
  auto answer_sessions = answer_.stack->Calls();
  ASSERT_TRUE(answer_sessions);
  // ActiveLocalCall is only the joined call — A must not be the active one.
  EXPECT_EQ((*active_b)->call_id, call_b);
  // One active call: placing B ended A on the offerer (Superseded); its Leave reached the answerer
  // before B's invite, so there A was ended by the peer.
  ExpectLive(offer_, call_a, LiveCallState::Ended, LiveCallEndReason::Superseded, "offer A");
  ExpectLive(answer_, call_a, LiveCallState::Ended, LiveCallEndReason::RemoteEnded, "answer A");
  ExpectLive(answer_, call_b, LiveCallState::Joined, LiveCallEndReason::None, "answer B");
  ExpectLive(offer_, call_b, LiveCallState::Joined, LiveCallEndReason::None, "offer B");

  FinishAnswerLeaveExpectBothIdle(call_b);
}

} // namespace
} // namespace pbr
