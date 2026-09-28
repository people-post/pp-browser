// B-GROUP-CALL (Tier B): three full CallStacks on one group call — product invite / accept wire,
// SoftMigrate 2→3 onto one media_relay hop, per-publisher audio on every side, leave. The hop is
// an in-process blind forwarder (subscribed streams only, never back to the sender); call-control
// is routed by each direct thread's peer. CALLS.md § Topology rules / § Soft-migrate 2→3.

#include "feature/conversations/tests/call_stack_compose_support.h"

#include "domain/mesh/l4/media_relay/IMediaRelayClient.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/SoftMigrateLogic.h"

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace pbr {
namespace {

using test::BuildStackSide;
using test::DestroyStackSide;
using test::DrainUntil;
using test::SoftStopStackSide;
using test::StackSide;
using test::StackSideOptions;
using test::StartCallNow;

// Not a built-in or retired org seed (NormalizeMeshConfig would swap those for the defaults).
constexpr char kHopPeerId[] = "12D3KooWGroupCallComposeMediaRelayHop";
constexpr char kHopMultiaddr[] = "/ip4/1.2.3.4/udp/443/adp/1.0.0/p2p/12D3KooWGroupCallComposeMediaRelayHop";
/** Audio frames (20 ms) each side must decode from each other publisher. */
constexpr uint64_t kMinRxFrames = 25;

class FakeHopClient;

/**
 * The media_relay hops every participant can reach, keyed by hop PeerId: a session per (hop, call
 * id); a frame goes to each other attached member of the sender's session that subscribed (stream,
 * channel) and started its reader — as the Amp coordinator forwards (never back to the sender).
 */
class FakeMediaRelayHop {
public:
  void Attach(const FakeHopClient* client, const std::string& hop_peer_id, const std::string& session_id,
              std::function<void(MediaDataFrame)> on_frame) {
    std::unique_lock lock(mu_);
    Member& m = members_[client];
    m = Member{};
    m.hop_peer_id = hop_peer_id;
    m.session_id = session_id;
    m.on_frame = std::move(on_frame);
  }

  void Detach(const FakeHopClient* client) {
    std::unique_lock lock(mu_);  // waits for in-flight deliveries to this member
    members_.erase(client);
  }

  void StartReading(const FakeHopClient* client) {
    std::unique_lock lock(mu_);
    if (auto it = members_.find(client); it != members_.end()) {
      it->second.reading = true;
    }
  }

  Roe<void> Subscribe(const FakeHopClient* client, uint32_t stream_id, uint16_t channel_id) {
    std::unique_lock lock(mu_);
    auto it = members_.find(client);
    if (it == members_.end()) {
      return Error("not attached");
    }
    it->second.subscriptions.insert(SubKey(stream_id, channel_id));
    return {};
  }

  Roe<void> Forward(const FakeHopClient* sender, const MediaDataFrame& frame) {
    std::shared_lock lock(mu_);
    const auto from = members_.find(sender);
    if (from == members_.end()) {
      return Error("not attached");
    }
    {
      std::lock_guard stats(stats_mu_);
      published_[sender].insert(frame.stream_id);
    }
    for (const auto& [client, member] : members_) {
      if (client == sender || member.hop_peer_id != from->second.hop_peer_id ||
          member.session_id != from->second.session_id || !member.reading ||
          member.subscriptions.count(SubKey(frame.stream_id, frame.channel_id)) == 0) {
        continue;
      }
      member.on_frame(frame);
    }
    return {};
  }

  bool IsAttached(const FakeHopClient* client) const {
    std::shared_lock lock(mu_);
    return members_.count(client) > 0;
  }

  /** True when the client is attached to `session_id` on hop `hop_peer_id`. */
  bool InSession(const FakeHopClient* client, const std::string& hop_peer_id, const std::string& session_id) const {
    std::shared_lock lock(mu_);
    const auto it = members_.find(client);
    return it != members_.end() && it->second.hop_peer_id == hop_peer_id && it->second.session_id == session_id;
  }

  /** Stream ids this client has published through the hop. */
  std::set<uint32_t> PublishedStreams(const FakeHopClient* client) const {
    std::lock_guard stats(stats_mu_);
    const auto it = published_.find(client);
    return it == published_.end() ? std::set<uint32_t>{} : it->second;
  }

private:
  struct Member {
    std::string hop_peer_id;
    std::string session_id;
    std::function<void(MediaDataFrame)> on_frame;
    bool reading = false;
    std::set<uint64_t> subscriptions;
  };

  static uint64_t SubKey(uint32_t stream_id, uint16_t channel_id) {
    return (static_cast<uint64_t>(stream_id) << 16) | channel_id;
  }

  mutable std::shared_mutex mu_;
  std::map<const FakeHopClient*, Member> members_;
  mutable std::mutex stats_mu_;
  std::map<const FakeHopClient*, std::set<uint32_t>> published_;
};

/** A participant's media_relay client, attached to the shared FakeMediaRelayHop. */
class FakeHopClient final : public IMediaRelayClient {
public:
  FakeHopClient(FakeMediaRelayHop& hop, std::string local_peer_id)
      : hop_(hop), local_peer_id_(std::move(local_peer_id)) {}

  Roe<std::string> LocalPeerIdBase58() const override { return local_peer_id_; }
  bool IsStarted() const override { return true; }

  Roe<MediaRelayQuote> RequestQuote(const std::string& hop_peer_key, const MediaRelayQuoteRequest& /*request*/,
                                    int /*timeout_ms*/) override {
    std::lock_guard lock(mu_);
    ++quote_calls_;
    quoted_hops_.push_back(hop_peer_key);
    MediaRelayQuote q;
    q.ok = true;
    q.quote_id = local_peer_id_ + "-quote-" + std::to_string(quote_calls_);
    q.a_up_bps = 64000;
    q.pricing_mode = "volunteer";
    return q;
  }

  Roe<MediaRelayAttachResult> AcceptAndAttach(const std::string& hop_peer_key, const std::string& /*quote_id*/,
                                              const std::string& session_id, const std::string& /*auth_stub*/,
                                              std::function<void(MediaDataFrame)> on_frame,
                                              int /*timeout_ms*/) override {
    hop_.Attach(this, hop_peer_key, session_id, std::move(on_frame));
    MediaRelayAttachResult r;
    r.ok = true;
    r.session_token = "tok-" + local_peer_id_;
    return r;
  }

  void StartClientFrameReader() override { hop_.StartReading(this); }

  Roe<MediaRelayAttachResult> AttachAsLocalHop(const std::string& /*session_id*/,
                                               std::function<void(MediaDataFrame)> /*on_frame*/) override {
    return Error("not a local hop");
  }

  Roe<void> Subscribe(uint32_t stream_id, uint16_t channel_id) override {
    return hop_.Subscribe(this, stream_id, channel_id);
  }
  Roe<void> SendFrame(const MediaDataFrame& frame) override { return hop_.Forward(this, frame); }
  void Detach() override { hop_.Detach(this); }
  bool IsAttached() const override { return hop_.IsAttached(this); }
  bool IsLocalHopAttached() const override { return false; }

  int QuoteCalls() const {
    std::lock_guard lock(mu_);
    return quote_calls_;
  }
  std::vector<std::string> QuotedHops() const {
    std::lock_guard lock(mu_);
    return quoted_hops_;
  }

private:
  FakeMediaRelayHop& hop_;
  const std::string local_peer_id_;
  mutable std::mutex mu_;
  int quote_calls_ = 0;
  std::vector<std::string> quoted_hops_;
};

class CallGroupStackComposeTest : public ::testing::Test {
protected:
  static constexpr size_t kA = 0;  // initiator (sticky hop picker)
  static constexpr size_t kB = 1;
  static constexpr size_t kC = 2;
  static constexpr size_t kSides = 3;

  void SetUp() override {
    EnsureSodiumInit();
    AppRuntime::Initialize(ManualOwnerRuntimeConfig());
    AppRuntime::InitializeUI();

    const char* tags[kSides] = {"group_a", "group_b", "group_c"};
    for (size_t i = 0; i < kSides; ++i) {
      relays_[i] = std::make_unique<FakeHopClient>(hop_, MeshPeerIdOf(i));
      // The hop is an org seed every participant knows (Wide scope → seed pick, V035).
      sides_[i].app_config.mesh.bootstrap_peers = {kHopMultiaddr};
      sides_[i].on_send = [this, i](const ThreadMessage& msg) { Route(i, msg); };
      StackSideOptions options;
      options.seed_dial_ok = true;
      options.relay = relays_[i].get();
      BuildStackSide(sides_[i], tags[i], static_cast<uint8_t>(0xa0 + 0x10 * i), options);
    }
    for (size_t i = 0; i < kSides; ++i) {
      for (size_t j = 0; j < kSides; ++j) {
        if (i == j) {
          continue;
        }
        sides_[i].dial->connected[sides_[j].local_identity] = true;
        sides_[i].dial->endpoints[sides_[j].local_identity] =
            "/ip4/127.0.0.1/udp/" + std::to_string(47200 + j) + "/adp/1.0.0/p2p/" + MeshPeerIdOf(j);
      }
      // A 1:1 call-media dial lands on the dialed side's inbound handler.
      sides_[i].transport->peer_inbound = [this, i](CallMediaDirectConnectParams params) {
        DeliverDirectHello(i, std::move(params));
      };
    }
  }

  void TearDown() override {
    // Soft-stop stacks first, then join AppRuntime before destroying stores (PR #216 follow-up).
    for (StackSide& side : sides_) {
      SoftStopStackSide(side);
    }
    AppRuntime::ShutdownUI();
    AppRuntime::Shutdown();
    for (StackSide& side : sides_) {
      DestroyStackSide(side);
    }
  }

  static std::string MeshPeerIdOf(size_t i) { return std::string("12D3KooWGroupSide") + static_cast<char>('A' + i); }

  /** Call-control from `from`: the direct thread's peer names the recipient. */
  void Route(size_t from, const ThreadMessage& msg) {
    auto thread = sides_[from].store->GetThread(msg.thread_id);
    if (!thread || !*thread) {
      ADD_FAILURE() << "call-control on unknown thread " << msg.thread_id;
      return;
    }
    const std::string& peer = (*thread)->peer_identity_value;
    for (size_t to = 0; to < kSides; ++to) {
      if (to != from && sides_[to].local_identity == peer) {
        std::lock_guard lock(wire_mu_);
        inboxes_[to].push_back({msg, sides_[from].local_identity});
        return;
      }
    }
    ADD_FAILURE() << "call-control to unknown peer '" << peer << "' on thread " << msg.thread_id;
  }

  void DeliverDirectHello(size_t from, CallMediaDirectConnectParams params) {
    for (size_t to = 0; to < kSides; ++to) {
      if (to == from || (params.peer_key != sides_[to].local_identity && params.peer_key != MeshPeerIdOf(to))) {
        continue;
      }
      test::FakeCallMediaTransport& target = *sides_[to].transport;
      if (!target.inbound.Installed()) {
        return;
      }
      // As on the wire: the inbound hello names the dialer.
      params.peer_key = sides_[from].local_identity;
      target.active = true;
      target.active_params = params;
      target.inbound.DeliverThen(std::move(params), [](CallMediaDirectConnectParams, CallMediaDirectCallbacks cbs) {
        if (cbs.on_connected) {
          cbs.on_connected();
        }
      });
      return;
    }
  }

  /** Deliver queued call-control to every stack; drain UI + owners between rounds. */
  void PumpWire(int rounds = 8) {
    for (int r = 0; r < rounds; ++r) {
      bool moved = false;
      for (size_t to = 0; to < kSides; ++to) {
        std::deque<Envelope> batch;
        {
          std::lock_guard lock(wire_mu_);
          batch.swap(inboxes_[to]);
        }
        for (Envelope& env : batch) {
          ASSERT_TRUE(sides_[to].inbound.apply_inbound_control(env.msg, env.sender, std::nullopt, std::nullopt));
          moved = true;
        }
      }
      AppRuntime::RunUIAndOwnerTasks();
      if (!moved) {
        break;
      }
    }
  }

  /** The invite's media key reaches invitees out of band (dual-stack fixture does the same). */
  void ShareMediaKey(const std::string& call_id) {
    auto key = sides_[kA].stack->MediaKeys()->LoadEpochKey(call_id, 1);
    ASSERT_TRUE(key && key->has_value());
    for (size_t i : {kB, kC}) {
      ASSERT_TRUE(sides_[i].stack->MediaKeys()->PutEpochKey(call_id, 1, **key));
    }
  }

  void AcceptInvite(size_t i, const std::string& call_id) {
    auto pending = sides_[i].ui->TopPendingInvite();
    ASSERT_TRUE(pending && pending->has_value()) << "invite should land on side " << i;
    ASSERT_EQ((*pending)->call_id, call_id);
    sides_[i].ui->Apply(CallLifecycleEvent::InviteSeen, call_id);
    sides_[i].ui->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  }

  /** Decoded audio frames side `rx` has from the streams side `tx` published through the hop. */
  uint64_t RxFrom(size_t rx, size_t tx) const {
    const std::set<uint32_t> streams = hop_.PublishedStreams(relays_[tx].get());
    const CallMediaEngine* engine = sides_[rx].stack->MediaEngine();
    if (streams.empty() || !engine) {
      return 0;
    }
    uint64_t n = 0;
    for (const CallMediaStreamHealth& s : engine->HealthSnapshot().streams) {
      if (streams.count(s.stream_id) > 0) {
        n += s.rx_frames;
      }
    }
    return n;
  }

  uint64_t RxAudioFrames(size_t i) const {
    const CallMediaEngine* engine = sides_[i].stack->MediaEngine();
    return engine ? engine->HealthSnapshot().rx_audio_frames : 0;
  }

  bool OnHopFor(size_t i, const std::string& call_id) const {
    return hop_.InSession(relays_[i].get(), kHopPeerId, call_id);
  }

  bool GroupLive(const std::string& call_id) const {
    for (size_t i = 0; i < kSides; ++i) {
      if (sides_[i].ui->Phase() != CallPhase::InCall || !OnHopFor(i, call_id)) {
        return false;
      }
      for (size_t j = 0; j < kSides; ++j) {
        if (j != i && RxFrom(i, j) < kMinRxFrames) {
          return false;
        }
      }
    }
    return true;
  }

  std::string Describe(const std::string& call_id) const {
    std::string out;
    for (size_t i = 0; i < kSides; ++i) {
      out += "\n  side " + std::string(1, static_cast<char>('A' + i)) +
             ": phase=" + std::to_string(static_cast<int>(sides_[i].ui->Phase())) +
             " on_hop=" + (OnHopFor(i, call_id) ? "1" : "0") + " quotes=" + std::to_string(relays_[i]->QuoteCalls()) +
             " rx_audio=" + std::to_string(RxAudioFrames(i));
      for (size_t j = 0; j < kSides; ++j) {
        if (j != i) {
          out += " rx_from_" + std::string(1, static_cast<char>('A' + j)) + "=" + std::to_string(RxFrom(i, j));
        }
      }
    }
    return out;
  }

  /** A starts a call from a group thread inviting B and C; returns the call id (media key shared). */
  std::string StartGroupCall() {
    Thread thread;
    // Windows: thread id is a directory name under threads/ — no ':'.
    thread.id = "thread-group-call";
    thread.kind = ThreadKind::Group;
    thread.title = "Group";
    thread.updated_at = util::NowUnixMs();
    EXPECT_TRUE(sides_[kA].store->UpsertThread(thread));

    auto started =
        StartCallNow(*sides_[kA].ui, thread.id, false, {sides_[kB].local_identity, sides_[kC].local_identity});
    EXPECT_TRUE(started) << (started ? "" : started.error().message);
    if (!started) {
      return {};
    }
    const std::string call_id = started->call_id;
    ShareMediaKey(call_id);
    PumpWire();
    return call_id;
  }

  /** Joined + direct media on both ends, and neither is on the hop (V050: 1:1 first). */
  bool DirectPairLive(size_t x, size_t y) const {
    for (size_t i : {x, y}) {
      const CallMediaEngine* engine = sides_[i].stack->MediaEngine();
      if (sides_[i].ui->Phase() != CallPhase::InCall || !engine || !engine->IsActive() || relays_[i]->IsAttached()) {
        return false;
      }
    }
    return true;
  }

  /**
   * `first` accepts (N=2 → direct 1:1 with A), then `second` (N=3 → SoftMigrate onto the hop).
   * With `together`, both accept before any call-control moves (simultaneous accepts).
   * Returns the call id once every side is InCall on the hop and decodes both other publishers.
   */
  std::string RunGroupCallToHopLive(size_t first = kB, size_t second = kC, bool together = false) {
    const std::string call_id = StartGroupCall();
    if (call_id.empty()) {
      return {};
    }
    AcceptInvite(first, call_id);
    if (!together) {
      DrainUntil([&]() {
        PumpWire();
        return DirectPairLive(kA, first);
      });
      EXPECT_TRUE(DirectPairLive(kA, first)) << "first accept must be a direct 1:1 (V050):" << Describe(call_id);
    }
    AcceptInvite(second, call_id);
    DrainUntil(
        [&]() {
          PumpWire();
          return GroupLive(call_id);
        },
        20000);
    EXPECT_TRUE(GroupLive(call_id)) << "group call never went live on the hop:" << Describe(call_id);
    return call_id;
  }

  /** Hop owner (re-picks) as side `i` computes it from its own rows (V050: earliest joined). */
  std::string OwnerSeenBy(size_t i, const std::string& call_id) const {
    CallSessionStore sessions(sides_[i].store->ProfileDbPath());
    auto rows = sessions.ListParticipants(call_id);
    if (!rows) {
      return {};
    }
    std::vector<SoftMigrateJoinedPeer> joined;
    for (const CallParticipant& p : *rows) {
      if (p.state == CallParticipantState::Joined) {
        joined.push_back({p.identity, p.joined_at});
      }
    }
    return SelectCallInitiator(joined);
  }

  struct Envelope {
    ThreadMessage msg;
    std::string sender;
  };

  FakeMediaRelayHop hop_;  // outlives the stacks (their relay clients point at it)
  std::unique_ptr<FakeHopClient> relays_[kSides];
  StackSide sides_[kSides];
  std::mutex wire_mu_;
  std::deque<Envelope> inboxes_[kSides];
};

// CALLS.md § Topology rules: N≥3 → SFU via media_relay; the sticky initiator picks the hop and
// fans out CallSfuAttach; every participant publishes once and hears every other publisher.
TEST_F(CallGroupStackComposeTest, ThreeWayCallSoftMigratesOntoOneHop) {
  const std::string call_id = RunGroupCallToHopLive();
  ASSERT_FALSE(call_id.empty());

  for (size_t i = 0; i < kSides; ++i) {
    EXPECT_EQ(sides_[i].ui->Phase(), CallPhase::InCall) << "side " << i;
    EXPECT_TRUE(OnHopFor(i, call_id)) << "side " << i << " not attached to the hop for the call";
    EXPECT_EQ(hop_.PublishedStreams(relays_[i].get()).size(), 1u)
        << "side " << i << " must publish one audio stream through the hop";
    for (size_t j = 0; j < kSides; ++j) {
      if (j != i) {
        EXPECT_GE(RxFrom(i, j), kMinRxFrames) << "side " << i << " does not hear side " << j;
      }
    }
  }
  // V021/V022: the initiator (earliest joined, session payer) quotes the hop it picked.
  ASSERT_GE(relays_[kA]->QuoteCalls(), 1);
  EXPECT_EQ(relays_[kA]->QuotedHops().front(), kHopPeerId);
}

// V050: every invitee's CallInvite carries the whole invite list, not the part StartCall had sent
// before it — the first invitee must know about the second.
TEST_F(CallGroupStackComposeTest, EveryInviteeSeesTheWholeInviteList) {
  const std::string call_id = StartGroupCall();
  ASSERT_FALSE(call_id.empty());
  for (size_t i : {kB, kC}) {
    CallSessionStore sessions(sides_[i].store->ProfileDbPath());  // read-only view of the stack's rows
    auto rows = sessions.ListParticipants(call_id);
    ASSERT_TRUE(rows) << rows.error().message;
    std::set<std::string> ids;
    for (const CallParticipant& p : *rows) {
      ids.insert(p.identity);
    }
    for (size_t j = 0; j < kSides; ++j) {
      EXPECT_EQ(ids.count(sides_[j].local_identity), 1u) << "side " << i << " roster misses side " << j;
    }
  }
}

// V050: planners arm on joined count, so whichever invitee accepts first gets the direct 1:1 —
// the path no longer depends on invite order (C was invited second).
TEST_F(CallGroupStackComposeTest, SecondInviteeAcceptingFirstGetsTheDirectPath) {
  const std::string call_id = RunGroupCallToHopLive(kC, kB);
  ASSERT_FALSE(call_id.empty());
  for (size_t i = 0; i < kSides; ++i) {
    EXPECT_TRUE(OnHopFor(i, call_id)) << "side " << i;
  }
}

// Both invitees accept before any call-control moves: each counts 2 joined from its own view, the
// initiator sees 3 — roster + CallSfuAttach must still bring everyone onto the one hop.
TEST_F(CallGroupStackComposeTest, SimultaneousAcceptsConvergeOnTheHop) {
  const std::string call_id = RunGroupCallToHopLive(kB, kC, /*together=*/true);
  ASSERT_FALSE(call_id.empty());
  for (size_t i = 0; i < kSides; ++i) {
    EXPECT_EQ(sides_[i].ui->Phase(), CallPhase::InCall) << "side " << i;
    EXPECT_TRUE(OnHopFor(i, call_id)) << "side " << i;
  }
}

// V050: the initiator leaving is not a re-evaluation either — the other two stay on the hop and keep
// hearing each other, and both name the same next owner of re-picks (earliest joined: B).
TEST_F(CallGroupStackComposeTest, InitiatorLeaveKeepsTheRestOnTheHop) {
  const std::string call_id = RunGroupCallToHopLive();
  ASSERT_FALSE(call_id.empty());
  EXPECT_EQ(OwnerSeenBy(kB, call_id), sides_[kA].local_identity);

  sides_[kA].ui->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  DrainUntil([&]() {
    PumpWire();
    return sides_[kA].ui->Phase() == CallPhase::Idle && !relays_[kA]->IsAttached() &&
           OwnerSeenBy(kB, call_id) == sides_[kB].local_identity &&
           OwnerSeenBy(kC, call_id) == sides_[kB].local_identity;
  });
  EXPECT_EQ(sides_[kA].ui->Phase(), CallPhase::Idle);
  EXPECT_FALSE(relays_[kA]->IsAttached()) << "the leaving initiator must detach from the hop";
  EXPECT_EQ(OwnerSeenBy(kB, call_id), sides_[kB].local_identity) << "B must see itself as the next owner";
  EXPECT_EQ(OwnerSeenBy(kC, call_id), sides_[kB].local_identity) << "C must agree B is the next owner";

  const uint64_t b_before = RxFrom(kB, kC);
  const uint64_t c_before = RxFrom(kC, kB);
  DrainUntil([&]() {
    PumpWire();
    return RxFrom(kB, kC) >= b_before + kMinRxFrames && RxFrom(kC, kB) >= c_before + kMinRxFrames;
  });
  for (size_t i : {kB, kC}) {
    EXPECT_EQ(sides_[i].ui->Phase(), CallPhase::InCall) << "side " << i;
    EXPECT_TRUE(OnHopFor(i, call_id)) << "side " << i << " left the hop after the initiator left";
  }
  EXPECT_GE(RxFrom(kB, kC), b_before + kMinRxFrames) << "B stopped hearing C:" << Describe(call_id);
  EXPECT_GE(RxFrom(kC, kB), c_before + kMinRxFrames) << "C stopped hearing B:" << Describe(call_id);
}

// A guest leaving a live group call ends only their media: the other two stay InCall and keep
// hearing each other; the initiator's Leave then ends the call everywhere.
TEST_F(CallGroupStackComposeTest, GuestLeaveKeepsRemainingPairThenInitiatorLeaveEndsAll) {
  const std::string call_id = RunGroupCallToHopLive();
  ASSERT_FALSE(call_id.empty());

  sides_[kC].ui->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  DrainUntil([&]() {
    PumpWire();
    return sides_[kC].ui->Phase() == CallPhase::Idle && !sides_[kC].stack->HasActiveLocalCall() &&
           !relays_[kC]->IsAttached();
  });
  EXPECT_EQ(sides_[kC].ui->Phase(), CallPhase::Idle);
  EXPECT_FALSE(sides_[kC].stack->HasActiveLocalCall());
  EXPECT_FALSE(relays_[kC]->IsAttached()) << "leaver must detach from the hop";

  const uint64_t a_before = RxAudioFrames(kA);
  const uint64_t b_before = RxAudioFrames(kB);
  DrainUntil([&]() {
    PumpWire();
    return RxAudioFrames(kA) >= a_before + kMinRxFrames && RxAudioFrames(kB) >= b_before + kMinRxFrames;
  });
  EXPECT_EQ(sides_[kA].ui->Phase(), CallPhase::InCall);
  EXPECT_EQ(sides_[kB].ui->Phase(), CallPhase::InCall);
  EXPECT_GE(RxAudioFrames(kA), a_before + kMinRxFrames) << "A stopped hearing B after C left:" << Describe(call_id);
  EXPECT_GE(RxAudioFrames(kB), b_before + kMinRxFrames) << "B stopped hearing A after C left:" << Describe(call_id);

  sides_[kA].ui->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  DrainUntil([&]() {
    PumpWire();
    for (size_t i = 0; i < kSides; ++i) {
      if (sides_[i].ui->Phase() != CallPhase::Idle || sides_[i].stack->HasActiveLocalCall() ||
          relays_[i]->IsAttached()) {
        return false;
      }
    }
    return true;
  });
  for (size_t i = 0; i < kSides; ++i) {
    EXPECT_EQ(sides_[i].ui->Phase(), CallPhase::Idle) << "side " << i;
    EXPECT_FALSE(sides_[i].stack->HasActiveLocalCall()) << "side " << i;
    EXPECT_FALSE(relays_[i]->IsAttached()) << "side " << i << " still attached to the hop";
  }
}

} // namespace
} // namespace pbr
