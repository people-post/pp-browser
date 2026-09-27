#pragma once

#include "domain/media/CallMediaEngine.h"
#include "domain/mesh/l4/media_relay/MediaRelayAttach.h"
#include "domain/messaging/PeerAnnounceTypes.h"
#include "foundation/runtime/DeferredSelf.h"

#include "common/Error.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Going live: which program, on which topic, through which relays (first reachable one publishes). */
struct BroadcastLiveRequest {
  std::string topic_id;
  std::string program_id;
  /** media_relay hops in preference order; the rest become the tip's L1 hints. */
  std::vector<std::string> hops;
};

/** What the ticket server needs to mint viewer tickets for the live program. */
struct BroadcastProgramKey {
  std::string publisher_peer_id;
  ByteVector media_key;
  uint32_t media_epoch = 1;
  /** Hop viewers should try first (ticket hint). */
  std::string hop_peer_id;
};

/** A program tip to sign and announce (Live on go-live / hop change, Ended on end). */
struct BroadcastTipDraft {
  std::string topic_id;
  std::string program_id;
  PeerAnnounceState state = PeerAnnounceState::Live;
  std::string join_handle;
  std::string hop_peer_id;
  std::vector<std::string> l1_hop_peer_ids;
};

/** What the broadcaster needs from the process (wired by the app; faked in gtests). */
struct BroadcasterPorts {
  std::function<std::string()> local_peer_id;
  /** 32 fresh random bytes per show (B004: one stable key per show). */
  std::function<ByteVector()> new_media_key;
  /** Fresh opaque join handle per show. */
  std::function<std::string(const std::string& program_id)> new_join_handle;
  std::function<void(const std::string& program_id, const std::string& join_handle, BroadcastProgramKey key)>
      put_program_key;
  std::function<void(const std::string& program_id, const std::string& join_handle)> clear_program_key;
  /** Sign + publish a tip (local feed; push to followers where wired). */
  std::function<Roe<void>(const BroadcastTipDraft& draft)> announce;
  MediaRelayAttachPorts relay;
  std::function<std::string(const std::string& hop_peer_id)> hop_multiaddr;
  /** Capture-only session (mic lease). */
  CallMediaEngine* engine = nullptr;
  std::function<void(std::function<void()>)> post_ui;
  std::function<void(std::chrono::milliseconds, std::function<void()>)> post_ui_after;
};

/**
 * Publishes one live program (media-client-layers l5, B004): fresh key + join handle → program key
 * to the ticket server → attach to the first reachable relay (l2) → capture-only engine whose
 * frames are sealed under the broadcast label and sent on `BroadcastPublisherStreamId(self)` →
 * Live tip. On relay loss it re-attaches (same hop first, then the others; re-announces when the
 * hop changes). End announces Ended, clears the key, detaches and stops capture. No call objects.
 *
 * Threading: public methods and state on the UI thread; completions posted to UI and dropped once
 * End / a newer GoLive invalidated them. Frames are sealed and sent on the engine's capture thread.
 */
class BroadcasterWorkflow {
public:
  enum class Phase { Idle, Attaching, Live, Recovering, Failed };
  struct Status {
    Phase phase = Phase::Idle;
    std::string topic_id;
    std::string program_id;
    std::string join_handle;
    std::string hop;
    std::string error;
    int reattaches = 0;
    uint64_t frames_sent = 0;
  };

  static constexpr int kMaxConsecutiveLosses = 3;

  explicit BroadcasterWorkflow(BroadcasterPorts ports);
  ~BroadcasterWorkflow();
  BroadcasterWorkflow(const BroadcasterWorkflow&) = delete;
  BroadcasterWorkflow& operator=(const BroadcasterWorkflow&) = delete;

  /** Start publishing (ends any current show first). Errors only for an unusable request. */
  Roe<void> GoLive(BroadcastLiveRequest request);
  /** End the show: Ended tip, key cleared, relay detached, capture stopped. */
  void End();
  /** Status with the live frame counter. */
  Status CurrentStatus() const;
  void SetOnStatusChanged(std::function<void()> callback) { on_status_changed_ = std::move(callback); }

  static const char* PhaseName(Phase phase);

private:
  struct Sender;

  void SetPhase(Phase phase, std::string hop = {});
  void Fail(const std::string& error);
  void Teardown(bool announce_end);
  void AttachNext(const std::string& why);
  void OnAttached(const std::string& hop, Roe<MediaRelayAttached> attached);
  void StartPublishing(const std::string& hop);
  Roe<void> Announce(PeerAnnounceState state);
  void OnSessionEnded(MediaRelayClientLoss loss);

  BroadcasterPorts ports_;
  Status status_;
  std::function<void()> on_status_changed_;
  DeferredSelf deferred_;

  // Per show (reset by Teardown).
  std::string self_peer_id_;
  std::vector<std::string> hops_;
  size_t next_hop_ = 0;
  ByteVector media_key_;
  std::shared_ptr<Sender> sender_;
  bool attached_ = false;
  bool engine_started_ = false;
  bool key_published_ = false;
  bool announced_live_ = false;
  std::string announced_hop_;
  uint64_t lost_observer_ = 0;
  int consecutive_losses_ = 0;
};

} // namespace pbr
