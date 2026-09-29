#pragma once

#include "domain/media/CallMediaEngine.h"
#include "domain/mesh/media_plane/MediaRelayAttach.h"
#include "domain/messaging/BroadcastRpcCodec.h"
#include "domain/messaging/BroadcastViewerLadder.h"
#include "domain/messaging/PeerAnnounceTypes.h"
#include "foundation/runtime/DeferredSelf.h"

#include "common/Error.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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
 * thread — the workflow hops to its owner through `post_owner`.
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
  /** Onto the thread that owns the workflow (the media-sessions owner in the product). */
  std::function<void(std::function<void()>)> post_owner;
  std::function<void(std::chrono::milliseconds, std::function<void()>)> post_owner_after;
  std::function<int64_t()> now_ms;
};

/**
 * Receive-only viewer of one live program (media-client-layers L013): reach publisher → ticket →
 * verify + key → admit-or-redirect ladder → attach to the admitting hop (l2) → subscribe the
 * publisher stream → playback-only engine session. Re-admits from the ladder on relay loss
 * (bounded, backed off). No call objects, no ringing.
 *
 * Threading: every public method and all state on the owner (`post_owner`: the media-sessions
 * owner); completions are posted there and dropped once Stop / a newer Watch invalidated them.
 * Frames are opened on the mesh IO thread and handed to the engine (thread-safe).
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
  const Status& CurrentStatus() const { return status_; }
  /** On the owner, after every phase change. */
  void SetOnStatusChanged(std::function<void()> callback) { on_status_changed_ = std::move(callback); }

  static const char* PhaseName(Phase phase);

private:
  struct FrameSink;

  void SetPhase(Phase phase, std::string hop = {});
  void Fail(const std::string& error);
  void Teardown();
  void PostUi(std::function<void()> task);

  void FetchTicket();
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
  DeferredSelf deferred_;

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
