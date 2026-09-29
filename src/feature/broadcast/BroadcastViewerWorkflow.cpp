#include "feature/broadcast/BroadcastViewerWorkflow.h"

#include "domain/mesh/l4/media_relay/client/MediaRelayFrameCrypto.h"
#include "domain/messaging/BroadcastJoinTicket.h"
#include "domain/messaging/BroadcastMedia.h"

#include "common/Logger.h"

#include <algorithm>
#include <atomic>
#include <utility>

namespace pbr {
namespace {

logging::Logger& ViewerLog() {
  static logging::Logger log = logging::getLogger("BroadcastViewer");
  return log;
}

constexpr uint16_t kAudioChannel = 0;
constexpr int64_t kAudioDownBps = 64000;
constexpr std::chrono::milliseconds kRecoveryBackoff{500};

/**
 * Wrap an owner handler for a completion that may arrive on any thread: hop to the owner, then run
 * only while `deferred`'s generation is unchanged (no Stop / newer Watch since).
 */
template <typename T>
std::function<void(T)> OnOwner(const std::function<void(std::function<void()>)>& post_owner, const DeferredSelf& deferred,
                            std::function<void(T)> handler) {
  return [post_owner, token = deferred.token(), snap = deferred.Snapshot(),
          handler = std::move(handler)](T value) {
    auto held = std::make_shared<T>(std::move(value));
    post_owner([token, snap, handler, held]() {
      if (DeferredSelf::Alive(token, snap)) {
        handler(std::move(*held));
      }
    });
  };
}

} // namespace

Roe<BroadcastWatchTarget> BroadcastWatchTargetFromTip(const PeerAnnounceTip& tip) {
  if (!TipIsProgramKind(tip)) {
    return Error("not a program tip");
  }
  if (tip.state != PeerAnnounceState::Live) {
    return Error("program is not live");
  }
  if (tip.peer_id.empty() || tip.program_id.empty() || tip.join_handle.empty()) {
    return Error("live tip without publisher / program / join handle");
  }
  BroadcastWatchTarget target;
  target.publisher_peer_id = tip.peer_id;
  target.program_id = tip.program_id;
  target.join_handle = tip.join_handle;
  if (!tip.hop_peer_id.empty()) {
    target.hops.push_back(tip.hop_peer_id);
  }
  for (const auto& hop : tip.l1_hop_peer_ids) {
    if (!hop.empty() && std::find(target.hops.begin(), target.hops.end(), hop) == target.hops.end()) {
      target.hops.push_back(hop);
    }
  }
  return target;
}

/** Immutable per attach; `live` gates delivery. Runs on the mesh IO thread. */
struct BroadcastViewerWorkflow::FrameSink {
  std::atomic<bool> live{false};
  ByteVector key;
  std::string context;
  uint32_t epoch = 1;
  uint32_t stream_id = 0;
  CallMediaEngine* engine = nullptr;
  std::atomic<uint64_t> rejected{0};

  void OnFrame(const MediaDataFrame& frame) {
    if (!live.load(std::memory_order_acquire) || frame.stream_id != stream_id) {
      return;
    }
    auto opened = OpenMediaRelayFrame(key, context, epoch, stream_id, static_cast<uint8_t>(frame.channel_id),
                                      frame.payload);
    if (!opened) {
      rejected.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    CallMediaEngine::SfuPacket packet;
    packet.stream_id = stream_id;
    packet.channel_id = frame.channel_id;
    packet.seq = opened->seq;
    packet.mark = opened->mark;
    packet.payload = std::move(opened->payload);
    engine->OnSfuPacket(packet);
  }
};

const char* BroadcastViewerWorkflow::PhaseName(Phase phase) {
  switch (phase) {
  case Phase::Idle:
    return "Idle";
  case Phase::Ticket:
    return "Ticket";
  case Phase::Admission:
    return "Admission";
  case Phase::Attaching:
    return "Attaching";
  case Phase::Listening:
    return "Listening";
  case Phase::Recovering:
    return "Recovering";
  case Phase::Failed:
    return "Failed";
  }
  return "?";
}

BroadcastViewerWorkflow::BroadcastViewerWorkflow(BroadcastViewerPorts ports) : ports_(std::move(ports)) {}

BroadcastViewerWorkflow::~BroadcastViewerWorkflow() {
  deferred_.Invalidate();
  Teardown();
}

void BroadcastViewerWorkflow::PostUi(std::function<void()> task) {
  deferred_.Post(ports_.post_owner, std::move(task));
}

void BroadcastViewerWorkflow::SetPhase(Phase phase, std::string hop) {
  status_.phase = phase;
  status_.hop = std::move(hop);
  ViewerLog().info << "phase=" << PhaseName(phase) << " program=" << status_.target.program_id
                   << " hop=" << status_.hop;
  if (on_status_changed_) {
    on_status_changed_();
  }
}

Roe<void> BroadcastViewerWorkflow::Watch(BroadcastWatchTarget target) {
  if (target.publisher_peer_id.empty() || target.program_id.empty() || target.join_handle.empty()) {
    return Error("watch target needs publisher, program and join handle");
  }
  if (!ports_.engine || !ports_.relay.relay || !ports_.relay.dial || !ports_.request_ticket || !ports_.post_owner) {
    return Error("broadcast viewing unavailable (media relay / engine not wired)");
  }
  deferred_.Invalidate();
  Teardown();
  status_ = Status{};
  status_.target = std::move(target);
  FetchTicket();
  return {};
}

void BroadcastViewerWorkflow::Stop() {
  deferred_.Invalidate();
  Teardown();
  const bool was_idle = status_.phase == Phase::Idle;
  status_ = Status{};
  if (!was_idle && on_status_changed_) {
    on_status_changed_();
  }
}

void BroadcastViewerWorkflow::Fail(const std::string& error) {
  ViewerLog().warning << "watch failed program=" << status_.target.program_id << ": " << error;
  deferred_.Invalidate();
  Teardown();
  status_.error = error;
  SetPhase(Phase::Failed, status_.hop);
}

void BroadcastViewerWorkflow::Teardown() {
  if (sink_) {
    sink_->live.store(false, std::memory_order_release);
  }
  IMediaRelayClient* relay = ports_.relay.relay;
  if (relay && lost_observer_ != 0) {
    relay->RemoveClientTransportLostObserver(lost_observer_);
  }
  lost_observer_ = 0;
  if (relay && attached_) {
    relay->Detach();
  }
  attached_ = false;
  if (engine_started_ && ports_.engine) {
    ports_.engine->Stop();
  }
  engine_started_ = false;
  consecutive_losses_ = 0;
  sink_.reset();
  ladder_.reset();
  ticket_json_.clear();
  ticket_hop_.clear();
  media_key_.clear();
}

// --- ticket -----------------------------------------------------------------------------------

void BroadcastViewerWorkflow::FetchTicket() {
  SetPhase(Phase::Ticket);
  const std::string publisher = status_.target.publisher_peer_id;
  auto request = [this, publisher]() {
    BroadcastTicketRequest req;
    req.program_id = status_.target.program_id;
    req.join_handle = status_.target.join_handle;
    req.viewer_peer_id = ports_.local_peer_id ? ports_.local_peer_id() : std::string();
    ports_.request_ticket(publisher, req,
                          OnOwner<Roe<BroadcastTicketResponse>>(
                              ports_.post_owner, deferred_,
                              [this](Roe<BroadcastTicketResponse> response) { OnTicket(std::move(response)); }));
  };
  if (!ports_.reach_peer) {
    request();
    return;
  }
  ports_.reach_peer(publisher, OnOwner<Roe<void>>(ports_.post_owner, deferred_, [this, request](Roe<void> reached) {
                      if (!reached) {
                        Fail("publisher unreachable: " + reached.error().message);
                        return;
                      }
                      request();
                    }));
}

void BroadcastViewerWorkflow::OnTicket(Roe<BroadcastTicketResponse> response) {
  if (!response) {
    Fail("ticket request failed: " + response.error().message);
    return;
  }
  if (!response->ok || !response->ticket) {
    Fail("publisher refused a ticket: " + (response->error.empty() ? std::string("no ticket") : response->error));
    return;
  }
  const BroadcastJoinTicket& ticket = *response->ticket;
  const auto& target = status_.target;
  if (ticket.program_id != target.program_id || ticket.join_handle != target.join_handle ||
      ticket.publisher_peer_id != target.publisher_peer_id) {
    Fail("ticket is for another program");
    return;
  }
  const auto publisher_key = ports_.publisher_key ? ports_.publisher_key(target.publisher_peer_id) : std::nullopt;
  if (!publisher_key) {
    Fail("publisher key unknown (no signed announce)");
    return;
  }
  const int64_t now = ports_.now_ms ? ports_.now_ms() : 0;
  const std::string viewer = ports_.local_peer_id ? ports_.local_peer_id() : std::string();
  auto key = ExtractBroadcastMediaKey(ticket, *publisher_key, now, viewer);
  if (!key) {
    Fail("ticket rejected: " + key.error().message);
    return;
  }
  auto json = EncodeBroadcastJoinTicketJson(ticket);
  if (!json) {
    Fail("ticket encode: " + json.error().message);
    return;
  }
  media_key_ = key->key_bytes;
  media_epoch_ = key->media_epoch;
  ticket_json_ = std::move(*json);
  ticket_hop_ = ticket.hop_peer_id;
  ladder_ = std::make_unique<BroadcastViewerLadder>(MakeLadder());
  RunLadder(ladder_->Start());
}

// --- admission ladder ---------------------------------------------------------------------------

BroadcastViewerLadder BroadcastViewerWorkflow::MakeLadder() const {
  std::vector<std::string> candidates = status_.target.hops;
  if (!ticket_hop_.empty()) {
    candidates.push_back(ticket_hop_);
  }
  return BroadcastViewerLadder(std::move(candidates));
}

void BroadcastViewerWorkflow::RunLadder(BroadcastViewerLadder::Step step) {
  switch (step.action) {
  case BroadcastViewerLadder::Action::Ask:
    AskAdmission(step.hop);
    return;
  case BroadcastViewerLadder::Action::Attach:
    Attach(step.hop);
    return;
  case BroadcastViewerLadder::Action::GiveUp:
    Fail("no relay admitted this viewer: " + step.reason);
    return;
  }
}

void BroadcastViewerWorkflow::AskAdmission(const std::string& hop) {
  SetPhase(Phase::Admission, hop);
  if (!ports_.request_admission) {
    RunLadder(ladder_->OnNoAdmissionService(hop));
    return;
  }
  BroadcastViewerAttachRequest request;
  request.program_id = status_.target.program_id;
  request.join_handle = status_.target.join_handle;
  request.viewer_peer_id = ports_.local_peer_id ? ports_.local_peer_id() : std::string();
  request.ticket_json = ticket_json_;
  request.redirect_budget = ladder_->RedirectBudget();
  request.path_stamp = ladder_->PathStamp();
  ports_.request_admission(hop, request,
                           OnOwner<Roe<BroadcastViewerAttachResult>>(
                               ports_.post_owner, deferred_, [this, hop](Roe<BroadcastViewerAttachResult> result) {
                                 OnAdmission(hop, std::move(result));
                               }));
}

void BroadcastViewerWorkflow::OnAdmission(const std::string& hop, Roe<BroadcastViewerAttachResult> result) {
  if (!result) {
    ViewerLog().info << "hop " << hop << " has no admission service (" << result.error().message
                     << ") — attaching directly";
    RunLadder(ladder_->OnNoAdmissionService(hop));
    return;
  }
  switch (result->action) {
  case BroadcastLadderViewerAction::Admit:
    RunLadder(ladder_->OnAdmitted(hop, result->admitted_hop_peer_id));
    return;
  case BroadcastLadderViewerAction::Redirect:
    RunLadder(ladder_->OnRedirect(hop, result->redirect_peer_ids, result->redirect_budget_remaining));
    return;
  case BroadcastLadderViewerAction::Refuse:
    RunLadder(ladder_->OnRefused(hop, result->refuse_reason));
    return;
  }
}

// --- attach + listen ----------------------------------------------------------------------------

void BroadcastViewerWorkflow::Attach(const std::string& hop) {
  IMediaRelayClient* relay = ports_.relay.relay;
  if (relay->IsAttached() || relay->IsLocalHopAttached()) {
    // One media_relay client session per mesh host today (L013); a call holds it.
    Fail("media relay client in use by a call");
    return;
  }
  SetPhase(Phase::Attaching, hop);
  const auto& target = status_.target;
  sink_ = std::make_shared<FrameSink>();
  sink_->key = media_key_;
  sink_->context = BroadcastMediaFrameContext(target.program_id, target.join_handle);
  sink_->epoch = media_epoch_;
  sink_->stream_id = BroadcastPublisherStreamId(target.publisher_peer_id);
  sink_->engine = ports_.engine;

  MediaRelayAttachRequest request;
  request.hop_peer_id = hop;
  request.hop_multiaddr = ports_.hop_multiaddr ? ports_.hop_multiaddr(hop) : std::string();
  request.session_id = target.join_handle;
  request.auth = target.join_handle;
  request.quote.session_id = target.join_handle;
  request.quote.participants = 1;
  request.quote.want_up_bps = 0;
  request.quote.want_down_bps = kAudioDownBps;

  MediaRelayAttachHooks hooks;
  hooks.accept_quote = [](const MediaRelayQuote& quote) -> Roe<void> {
    if (quote.rate > 0) {
      return Error("paid relays are not supported for broadcast yet");
    }
    return {};
  };
  hooks.still_wanted = [token = deferred_.token(), snap = deferred_.Snapshot()]() {
    return DeferredSelf::Alive(token, snap);
  };
  hooks.on_frame = [sink = sink_](MediaDataFrame frame) { sink->OnFrame(frame); };

  auto on_owner = OnOwner<Roe<MediaRelayAttached>>(ports_.post_owner, deferred_, [this, hop](Roe<MediaRelayAttached> attached) {
    OnAttached(hop, std::move(attached));
  });
  // A Stop while AcceptAndAttach is on the wire drops `on_owner`; the relay may still attach — detach
  // it then so the client session does not leak. (A call attaching in that same window would be
  // detached too; l4c stops watching before a call takes media.)
  AttachToMediaRelayAsync(ports_.relay, std::move(request), std::move(hooks),
                          [relay, on_owner, token = deferred_.token(), snap = deferred_.Snapshot()](
                              Roe<MediaRelayAttached> attached) {
                            if (attached && !DeferredSelf::Alive(token, snap)) {
                              relay->Detach();
                              return;
                            }
                            on_owner(std::move(attached));
                          });
}

void BroadcastViewerWorkflow::OnAttached(const std::string& hop, Roe<MediaRelayAttached> attached) {
  if (!attached) {
    ViewerLog().info << "attach to " << hop << " failed: " << attached.error().message;
    RunLadder(ladder_->OnAttachFailed(hop, attached.error().message));
    return;
  }
  attached_ = true;
  StartListening(hop);
}

void BroadcastViewerWorkflow::StartListening(const std::string& hop) {
  IMediaRelayClient* relay = ports_.relay.relay;
  if (auto started = ports_.engine->Start(status_.target.join_handle, CallMediaEngine::SessionSpec::PlaybackOnly(), {});
      !started) {
    Fail("playback: " + started.error().message);
    return;
  }
  engine_started_ = true;
  relay->StartClientFrameReader();
  if (auto subscribed = relay->Subscribe(sink_->stream_id, kAudioChannel); !subscribed) {
    Fail("subscribe: " + subscribed.error().message);
    return;
  }
  if (lost_observer_ == 0) {
    lost_observer_ = relay->AddClientTransportLostObserver(
        [post_owner = ports_.post_owner, token = deferred_.token(), snap = deferred_.Snapshot(),
         this](MediaRelayClientLoss loss) {
          post_owner([token, snap, this, loss]() {
            if (DeferredSelf::Alive(token, snap)) {
              OnSessionEnded(loss);
            }
          });
        });
  }
  sink_->live.store(true, std::memory_order_release);
  consecutive_losses_ = 0;
  SetPhase(Phase::Listening, hop);
}

// --- recovery -------------------------------------------------------------------------------------

void BroadcastViewerWorkflow::OnSessionEnded(MediaRelayClientLoss loss) {
  if (status_.phase != Phase::Listening || !attached_) {
    return;
  }
  // Our observer is removed before our own Detach, so any end here is not ours. Transport loss →
  // re-admit; replaced / detached by a call → re-admission finds the client busy and fails clearly.
  ViewerLog().info << "relay session ended (" << (loss == MediaRelayClientLoss::TransportLost ? "lost"
                                                  : loss == MediaRelayClientLoss::Replaced    ? "replaced"
                                                                                              : "detached")
                   << ") program=" << status_.target.program_id;
  Recover();
}

void BroadcastViewerWorkflow::Recover() {
  attached_ = false;
  if (sink_) {
    sink_->live.store(false, std::memory_order_release);
  }
  if (consecutive_losses_ >= kMaxConsecutiveLosses) {
    Fail("relay connection lost (" + std::to_string(consecutive_losses_) + " re-admissions in a row failed)");
    return;
  }
  ++consecutive_losses_;
  ++status_.recoveries;
  SetPhase(Phase::Recovering, status_.hop);
  auto readmit = [this]() {
    ladder_ = std::make_unique<BroadcastViewerLadder>(MakeLadder());
    RunLadder(ladder_->Start());
  };
  const auto backoff = kRecoveryBackoff * consecutive_losses_;
  if (ports_.post_owner_after) {
    ports_.post_owner_after(backoff, deferred_.Bind(readmit));
  } else {
    PostUi(readmit);
  }
}

} // namespace pbr
