#pragma once

#include "domain/media/CallMediaEngine.h"
#include "domain/mesh/media_plane/MediaRelayAttach.h"
#include "domain/messaging/BroadcastRpcCodec.h"
#include "domain/messaging/BroadcastViewerLadder.h"
#include "domain/messaging/PeerAnnounceTypes.h"
#include "foundation/runtime/OwnerOutbox.h"

#include "common/Error.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What a viewer watches: one live program of one publisher (from a signed, Live tip). */
struct BroadcastWatchTarget {
  std::string publisher_peer_id;
  std::string program_id;
  std::string join_handle;
  /** Hops to ask, in order: the tip's primary hop, then its L1 hints. */
  std::vector<std::string> hops;
};

/** Refuses tips that are not a Live program with a join handle and publisher. */
Roe<BroadcastWatchTarget> BroadcastWatchTargetFromTip(const PeerAnnounceTip& tip);

/**
 * What the viewer needs from the process, in its own words. Wired by the app from neutral mesh
 * objects and the broadcast RPC client; faked in gtests. Async completions may arrive on any
 * thread — the workflow turns them into its own events.
 */
struct BroadcastViewerPorts {
  /** Mesh PeerId tickets bind to. */
  std::function<std::string()> local_peer_id;
  /** Publisher's ML-DSA public key, from its signed announce; nullopt when unknown. */
  std::function<std::optional<ByteVector>(const std::string& publisher_peer_id)> publisher_key;
  /** Link to a peer (the publisher, for its ticket). */
  std::function<void(const std::string& peer_id, std::function<void(Roe<void>)> on_done)> reach_peer;
  std::function<void(const std::string& publisher_peer_id, const BroadcastTicketRequest& request,
                     std::function<void(Roe<BroadcastTicketResponse>)> on_done)>
      request_ticket;
  /** Admission RPC to a hop. An error (not a Refuse) means the hop has no admission service. */
  std::function<void(const std::string& hop_peer_id, const BroadcastViewerAttachRequest& request,
                     std::function<void(Roe<BroadcastViewerAttachResult>)> on_done)>
      request_admission;
  /** media_relay client + dial registry + service reach (L008 / L009). */
  MediaRelayAttachPorts relay;
  /** Dial hint for a hop (may return empty). */
  std::function<std::string(const std::string& hop_peer_id)> hop_multiaddr;
  /** Playback-only session target (owner for Start / Stop; OnSfuPacket any thread). */
  CallMediaEngine* engine = nullptr;
  std::function<int64_t()> now_ms;
};

/** What the viewer reports to itself through its runner (BroadcastHub); `watch` names the watch. */
namespace viewer_event {

struct Reached {
  uint64_t watch = 0;
  Roe<void> reached;
};
struct Ticket {
  uint64_t watch = 0;
  Roe<BroadcastTicketResponse> response;
};
struct Admission {
  uint64_t watch = 0;
  std::string hop;
  Roe<BroadcastViewerAttachResult> result;
};
struct Attached {
  uint64_t watch = 0;
  std::string hop;
  Roe<MediaRelayAttached> attached;
};
/** The relay client session ended (relay I/O). */
struct SessionEnded {
  uint64_t watch = 0;
  MediaRelayClientLoss loss = MediaRelayClientLoss::TransportLost;
};
/** The back-off before re-admission ended. */
struct ReadmitDue {
  uint64_t watch = 0;
};

} // namespace viewer_event

using ViewerEvent = std::variant<viewer_event::Reached, viewer_event::Ticket, viewer_event::Admission,
                                 viewer_event::Attached, viewer_event::SessionEnded, viewer_event::ReadmitDue>;

/**
 * Receive-only viewer of one live program (media-client-layers L013): reach publisher → ticket →
 * verify + key → admit-or-redirect ladder → attach to the admitting hop (l2) → subscribe the
 * publisher stream → playback-only engine session. Re-admits from the ladder on relay loss
 * (bounded, backed off). No call objects, no ringing.
 *
 * Threading: passive, on its runner's owner (THREADING.md § Owner runners). Results come back as
 * `ViewerEvent`s through its outbox, carrying the watch they belong to; one for an older watch
 * (Stop / a newer Watch since) is dropped. Frames are opened on the mesh IO thread and handed to
 * the engine (thread-safe).
 */
class BroadcastViewerWorkflow {
public:
  enum class Phase { Idle, Ticket, Admission, Attaching, Listening, Recovering, Failed };
  struct Status {
    Phase phase = Phase::Idle;
    BroadcastWatchTarget target;
    /** Hop being asked / attached / listened through. */
    std::string hop;
    std::string error;
    /** Re-admissions after relay loss, this watch. */
    int recoveries = 0;
  };

  /** Consecutive losses without reaching Listening again before the watch fails. */
  static constexpr int kMaxConsecutiveLosses = 3;

  explicit BroadcastViewerWorkflow(BroadcastViewerPorts ports);
  ~BroadcastViewerWorkflow();
  BroadcastViewerWorkflow(const BroadcastViewerWorkflow&) = delete;
  BroadcastViewerWorkflow& operator=(const BroadcastViewerWorkflow&) = delete;

  /** Start watching (stops any current watch first). Errors only for an unusable target. */
  Roe<void> Watch(BroadcastWatchTarget target);
  void Stop();
  /** Where its events go (the runner hands them back to `Handle` on the owner). */
  void SetOutbox(OwnerOutbox<ViewerEvent> outbox) { outbox_ = std::move(outbox); }
  void Handle(ViewerEvent& event);
  const Status& CurrentStatus() const { return status_; }
  /** On the owner, after every phase change. */
  void SetOnStatusChanged(std::function<void()> callback) { on_status_changed_ = std::move(callback); }

  static const char* PhaseName(Phase phase);

private:
  struct FrameSink;

  void SetPhase(Phase phase, std::string hop = {});
  void Fail(const std::string& error);
  void Teardown();
  /** A new watch: results of the previous one are stale from here on. */
  void NewWatch();
  bool Current(uint64_t watch) const { return watch == watch_; }

  void FetchTicket();
  void RequestTicket();
  void OnTicket(Roe<BroadcastTicketResponse> response);
  void RunLadder(BroadcastViewerLadder::Step step);
  void AskAdmission(const std::string& hop);
  void OnAdmission(const std::string& hop, Roe<BroadcastViewerAttachResult> result);
  void Attach(const std::string& hop);
  void OnAttached(const std::string& hop, Roe<MediaRelayAttached> attached);
  void StartListening(const std::string& hop);
  void OnSessionEnded(MediaRelayClientLoss loss);
  void Recover();
  BroadcastViewerLadder MakeLadder() const;

  BroadcastViewerPorts ports_;
  Status status_;
  std::function<void()> on_status_changed_;
  OwnerOutbox<ViewerEvent> outbox_;
  uint64_t watch_ = 0;
  /** `watch_` for I/O-side checks (attach still wanted). */
  std::shared_ptr<std::atomic<uint64_t>> current_watch_ = std::make_shared<std::atomic<uint64_t>>(0);
  OwnerExecutor::TimerId readmit_timer_ = 0;

  // Per watch (reset by Teardown).
  std::string ticket_json_;
  std::string ticket_hop_;
  ByteVector media_key_;
  uint32_t media_epoch_ = 1;
  std::unique_ptr<BroadcastViewerLadder> ladder_;
  std::shared_ptr<FrameSink> sink_;
  bool attached_ = false;
  bool engine_started_ = false;
  uint64_t lost_observer_ = 0;
  int consecutive_losses_ = 0;
};

} // namespace pbr
