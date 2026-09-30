#include "feature/broadcast/BroadcasterWorkflow.h"

#include "domain/mesh/l4/media_relay/client/MediaRelayFrameCrypto.h"
#include "domain/messaging/BroadcastMedia.h"

#include "common/Logger.h"

#include <algorithm>
#include <atomic>
#include <type_traits>
#include <utility>

namespace pbr {
namespace {

logging::Logger& BroadcasterLog() {
  static logging::Logger log = logging::getLogger("Broadcaster");
  return log;
}

constexpr int64_t kAudioUpBps = 64000;
constexpr std::chrono::milliseconds kRecoveryBackoff{500};

} // namespace

/** One per show; the engine's capture thread seals and sends through it while `live`. */
struct BroadcasterWorkflow::Sender {
  IMediaRelayClient* relay = nullptr;
  ByteVector key;
  std::string context;
  uint32_t epoch = 1;
  uint32_t stream_id = 0;
  std::atomic<bool> live{false};
  std::shared_ptr<std::atomic<uint64_t>> sent;  // the workflow's counter (outlives the show)

  void Send(const CallMediaEngine::SfuPacket& packet) {
    if (!live.load(std::memory_order_acquire)) {
      return;
    }
    const auto channel = static_cast<uint8_t>(packet.channel_id);
    auto body = SealMediaRelayFrame(key, context, epoch, stream_id, packet.seq, packet.mark, channel, packet.payload);
    if (!body) {
      return;
    }
    MediaDataFrame frame;
    frame.stream_id = stream_id;
    frame.channel_id = packet.channel_id;
    frame.channel_type = MediaChannelTypeFor(packet.channel_id);
    frame.seq = packet.seq;
    frame.mark = packet.mark;
    frame.payload = std::move(*body);
    if (relay->SendFrame(frame)) {
      sent->fetch_add(1, std::memory_order_relaxed);
    }
  }
};

const char* BroadcasterWorkflow::PhaseName(Phase phase) {
  switch (phase) {
  case Phase::Idle:
    return "Idle";
  case Phase::Attaching:
    return "Attaching";
  case Phase::Live:
    return "Live";
  case Phase::Recovering:
    return "Recovering";
  case Phase::Failed:
    return "Failed";
  }
  return "?";
}

BroadcasterWorkflow::BroadcasterWorkflow(BroadcasterPorts ports) : ports_(std::move(ports)) {}

BroadcasterWorkflow::~BroadcasterWorkflow() {
  NewShow();
  Teardown(/*announce_end=*/true);
}

void BroadcasterWorkflow::NewShow() {
  ++show_;
  current_show_->store(show_, std::memory_order_release);
  outbox_.Cancel(reattach_timer_);
}

void BroadcasterWorkflow::Handle(BroadcasterEvent& event) {
  std::visit(
      [this](auto& e) {
        using E = std::decay_t<decltype(e)>;
        if (e.show != show_) {
          return;  // an older show's result
        }
        if constexpr (std::is_same_v<E, broadcaster_event::Attached>) {
          OnAttached(e.hop, std::move(e.attached));
        } else if constexpr (std::is_same_v<E, broadcaster_event::Announced>) {
          OnAnnounced(e.first, std::move(e.announced));
        } else if constexpr (std::is_same_v<E, broadcaster_event::SessionEnded>) {
          OnSessionEnded(e.loss);
        } else if constexpr (std::is_same_v<E, broadcaster_event::ReattachDue>) {
          if (reattach_timer_ != 0) {
            reattach_timer_ = 0;
            AttachNext("relay lost");
          }
        } else {
          static_assert(!sizeof(E), "handle every BroadcasterEvent");
        }
      },
      event);
}

BroadcasterWorkflow::Status BroadcasterWorkflow::CurrentStatus() const {
  Status status = status_;
  status.frames_sent = frames_sent_->load(std::memory_order_relaxed);
  return status;
}

void BroadcasterWorkflow::SetPhase(Phase phase, std::string hop) {
  status_.phase = phase;
  status_.hop = std::move(hop);
  BroadcasterLog().info << "phase=" << PhaseName(phase) << " program=" << status_.program_id << " hop=" << status_.hop;
  if (on_status_changed_) {
    on_status_changed_();
  }
}

Roe<void> BroadcasterWorkflow::GoLive(BroadcastLiveRequest request) {
  if (request.program_id.empty()) {
    return Error("going live needs a program id");
  }
  std::vector<std::string> hops;
  for (auto& hop : request.hops) {
    if (!hop.empty() && std::find(hops.begin(), hops.end(), hop) == hops.end()) {
      hops.push_back(std::move(hop));
    }
  }
  if (hops.empty()) {
    return Error("going live needs at least one media relay");
  }
  if (!ports_.engine || !ports_.relay.relay || !ports_.relay.dial || !ports_.announce || !ports_.put_program_key ||
      !ports_.new_media_key || !ports_.new_join_handle || !outbox_.IsBound()) {
    return Error("broadcasting unavailable (media relay / engine / announce not wired)");
  }
  ByteVector key = ports_.new_media_key();
  if (key.size() != 32) {
    return Error("media key must be 32 bytes");
  }
  NewShow();
  Teardown(/*announce_end=*/true);
  status_ = Status{};
  status_.topic_id = request.topic_id;
  status_.program_id = request.program_id;
  status_.join_handle = ports_.new_join_handle(request.program_id);
  hops_ = std::move(hops);
  next_hop_ = 0;
  media_key_ = std::move(key);

  self_peer_id_ = ports_.local_peer_id ? ports_.local_peer_id() : std::string();
  const std::string& self = self_peer_id_;
  sender_ = std::make_shared<Sender>();
  frames_sent_->store(0, std::memory_order_relaxed);
  sender_->sent = frames_sent_;
  sender_->relay = ports_.relay.relay;
  sender_->key = media_key_;
  sender_->context = BroadcastMediaFrameContext(status_.program_id, status_.join_handle);
  sender_->stream_id = BroadcastPublisherStreamId(self);

  BroadcastProgramKey program_key{self, media_key_, 1, hops_.front()};
  ports_.put_program_key(status_.program_id, status_.join_handle, std::move(program_key));
  key_published_ = true;
  AttachNext({});
  return {};
}

void BroadcasterWorkflow::End() {
  NewShow();
  const bool was_idle = status_.phase == Phase::Idle;
  Teardown(/*announce_end=*/true);
  status_ = Status{};
  if (!was_idle && on_status_changed_) {
    on_status_changed_();
  }
}

void BroadcasterWorkflow::Fail(const std::string& error) {
  BroadcasterLog().warning << "broadcast failed program=" << status_.program_id << ": " << error;
  NewShow();
  Teardown(/*announce_end=*/true);
  status_.error = error;
  SetPhase(Phase::Failed, status_.hop);
}

void BroadcasterWorkflow::Teardown(bool announce_end) {
  if (sender_) {
    sender_->live.store(false, std::memory_order_release);
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
    ports_.engine->Stop();  // joins the capture thread: no Send runs after this
  }
  engine_started_ = false;
  if (announced_live_ && announce_end) {
    // Fire and forget: the workflow may be gone when it lands (the handler touches nothing of it).
    Announce(PeerAnnounceState::Ended, [](Roe<void> ended) {
      if (!ended) {
        BroadcasterLog().warning << "Ended tip not published: " << ended.error().message;
      }
    });
  }
  announced_live_ = false;
  announced_hop_.clear();
  if (key_published_ && ports_.clear_program_key) {
    ports_.clear_program_key(status_.program_id, status_.join_handle);
  }
  key_published_ = false;
  consecutive_losses_ = 0;
  hops_.clear();
  next_hop_ = 0;
  media_key_.clear();
}

void BroadcasterWorkflow::Announce(PeerAnnounceState state, std::function<void(Roe<void>)> on_done) {
  BroadcastTipDraft draft;
  draft.topic_id = status_.topic_id;
  draft.program_id = status_.program_id;
  draft.state = state;
  draft.join_handle = status_.join_handle;
  if (state == PeerAnnounceState::Live) {
    draft.hop_peer_id = status_.hop;
    for (const auto& hop : hops_) {
      if (hop != status_.hop) {
        draft.l1_hop_peer_ids.push_back(hop);
      }
    }
  }
  ports_.announce(draft, std::move(on_done));
}

// --- attach -----------------------------------------------------------------------------------

void BroadcasterWorkflow::AttachNext(const std::string& why) {
  if (next_hop_ >= hops_.size()) {
    Fail("no media relay accepted the broadcast" + (why.empty() ? std::string() : ": " + why));
    return;
  }
  IMediaRelayClient* relay = ports_.relay.relay;
  if (relay->IsAttached() || relay->IsLocalHopAttached()) {
    Fail("media relay client in use by a call");  // one client session per mesh host (L013)
    return;
  }
  const std::string hop = hops_[next_hop_++];
  SetPhase(status_.phase == Phase::Recovering ? Phase::Recovering : Phase::Attaching, hop);

  MediaRelayAttachRequest request;
  request.hop_peer_id = hop;
  request.hop_multiaddr = ports_.hop_multiaddr ? ports_.hop_multiaddr(hop) : std::string();
  request.session_id = status_.join_handle;
  request.auth = status_.join_handle;
  request.quote.session_id = status_.join_handle;
  request.quote.participants = 1;
  request.quote.want_up_bps = kAudioUpBps;
  request.quote.want_down_bps = 0;

  MediaRelayAttachHooks hooks;
  hooks.accept_quote = [](const MediaRelayQuote& quote) -> Roe<void> {
    if (quote.rate > 0) {
      return Error("paid relays are not supported for broadcast yet");
    }
    return {};
  };
  hooks.still_wanted = [current = current_show_, show = show_]() {
    return current->load(std::memory_order_acquire) == show;
  };
  hooks.on_frame = [](MediaDataFrame) {};  // publish-only: subscribes to nothing

  AttachToMediaRelayAsync(ports_.relay, std::move(request), std::move(hooks),
                          [relay, outbox = outbox_, current = current_show_, show = show_,
                           hop](Roe<MediaRelayAttached> attached) {
                            if (attached && current->load(std::memory_order_acquire) != show) {
                              relay->Detach();  // ended while AcceptAndAttach was on the wire
                              return;
                            }
                            outbox.Emit(broadcaster_event::Attached{show, hop, std::move(attached)});
                          });
}

void BroadcasterWorkflow::OnAttached(const std::string& hop, Roe<MediaRelayAttached> attached) {
  if (!attached) {
    BroadcasterLog().info << "attach to " << hop << " failed: " << attached.error().message;
    AttachNext("attach to " + hop + " failed: " + attached.error().message);
    return;
  }
  attached_ = true;
  StartPublishing(hop);
}

void BroadcasterWorkflow::StartPublishing(const std::string& hop) {
  if (!engine_started_) {
    auto sender = sender_;
    if (auto started = ports_.engine->Start(status_.join_handle, CallMediaEngine::SessionSpec::CaptureOnly(),
                                            [sender](const CallMediaEngine::SfuPacket& packet) { sender->Send(packet); });
        !started) {
      Fail("capture: " + started.error().message);
      return;
    }
    engine_started_ = true;
  }
  IMediaRelayClient* relay = ports_.relay.relay;
  if (lost_observer_ == 0) {
    lost_observer_ = relay->AddClientTransportLostObserver(
        [outbox = outbox_, show = show_](MediaRelayClientLoss loss) {
          outbox.Emit(broadcaster_event::SessionEnded{show, loss});
        });
  }
  sender_->live.store(true, std::memory_order_release);
  const bool first = !announced_live_;
  SetPhase(Phase::Live, hop);
  if (hop != announced_hop_) {
    // Tickets minted from now on point at the hop actually carrying the show.
    ports_.put_program_key(status_.program_id, status_.join_handle,
                           BroadcastProgramKey{self_peer_id_, media_key_, 1, hop});
    // Marked announced now: an End before the result still publishes Ended (after Live, FIFO).
    announced_live_ = true;
    announced_hop_ = hop;
    Announce(PeerAnnounceState::Live, [outbox = outbox_, show = show_, first](Roe<void> announced) {
      outbox.Emit(broadcaster_event::Announced{show, first, std::move(announced)});
    });
  }
  consecutive_losses_ = 0;
}

void BroadcasterWorkflow::OnAnnounced(const bool first, Roe<void> announced) {
  if (announced) {
    return;
  }
  if (first) {
    Fail("live tip not published: " + announced.error().message);
    return;
  }
  BroadcasterLog().warning << "hop-change tip not published: " << announced.error().message;
}

// --- recovery -------------------------------------------------------------------------------------

void BroadcasterWorkflow::OnSessionEnded(MediaRelayClientLoss loss) {
  if (status_.phase != Phase::Live || !attached_) {
    return;
  }
  attached_ = false;
  sender_->live.store(false, std::memory_order_release);
  BroadcasterLog().info << "relay session ended ("
                        << (loss == MediaRelayClientLoss::TransportLost ? "lost"
                            : loss == MediaRelayClientLoss::Replaced    ? "replaced"
                                                                        : "detached")
                        << ") program=" << status_.program_id;
  if (consecutive_losses_ >= kMaxConsecutiveLosses) {
    Fail("relay connection lost (" + std::to_string(consecutive_losses_) + " re-attaches in a row failed)");
    return;
  }
  ++consecutive_losses_;
  ++status_.reattaches;
  // Same hop first (keeps the tip valid), then the others.
  const std::string current = status_.hop;
  std::stable_partition(hops_.begin(), hops_.end(), [&current](const std::string& h) { return h == current; });
  next_hop_ = 0;
  SetPhase(Phase::Recovering, current);
  reattach_timer_ = outbox_.After(kRecoveryBackoff * consecutive_losses_, broadcaster_event::ReattachDue{show_});
}

} // namespace pbr
