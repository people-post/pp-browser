#pragma once

#include "domain/media/CallMediaEngine.h"
#include "domain/mesh/l4/media_relay/MediaRelayVideoLevels.h"
#include "domain/mesh/media_plane/MediaRelayAttach.h"
#include "domain/messaging/PeerAnnounceTypes.h"
#include "foundation/runtime/OwnerOutbox.h"

#include "common/Error.h"

#include <chrono>
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Going live: which program, on which topic, through which relays (first reachable one publishes). */
struct BroadcastLiveRequest {
  std::string topic_id;
  std::string program_id;
  /** media_relay hops in preference order; the rest become the tip's L1 hints. */
  std::vector<std::string> hops;
  /** Publish camera video too (B009: at the level the relay answers); false = audio only. */
  bool video = false;
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
  /** B009: the video levels published (empty = audio only). */
  std::vector<int> video_levels;
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
  /**
   * Sign + publish a tip (local feed; push to followers where wired). Asynchronous: the feed
   * belongs to another owner; `on_done` may run on any thread (the workflow reports it as an event).
   */
  std::function<void(const BroadcastTipDraft& draft, std::function<void(Roe<void>)> on_done)> announce;
  MediaRelayAttachPorts relay;
  std::function<std::string(const std::string& hop_peer_id)> hop_multiaddr;
  /** Capture-only session (mic lease; camera lease when publishing video). */
  CallMediaEngine* engine = nullptr;
  /** B009: the video levels this device can produce, and how many at once (a phone: one level). */
  MediaRelayVideoOffer video_offer{{kDefaultVideoLevel}, 1};
};

/** What the broadcaster reports to itself through its runner (BroadcastHub); `show` names the show. */
namespace broadcaster_event {

struct Attached {
  uint64_t show = 0;
  std::string hop;
  Roe<MediaRelayAttached> attached;
};
/** The Live tip's announce answered (`first`: the show's first Live tip). */
struct Announced {
  uint64_t show = 0;
  bool first = false;
  Roe<void> announced;
};
/** The relay client session ended (relay I/O). */
struct SessionEnded {
  uint64_t show = 0;
  MediaRelayClientLoss loss = MediaRelayClientLoss::TransportLost;
};
/** The back-off before re-attaching ended. */
struct ReattachDue {
  uint64_t show = 0;
};

} // namespace broadcaster_event

using BroadcasterEvent = std::variant<broadcaster_event::Attached, broadcaster_event::Announced,
                                      broadcaster_event::SessionEnded, broadcaster_event::ReattachDue>;

/**
 * Publishes one live program (media-client-layers l5, B004): fresh key + join handle → program key
 * to the ticket server → attach to the first reachable relay (l2) → capture-only engine whose
 * frames are sealed under the broadcast label and sent on `BroadcastPublisherStreamId(self)` →
 * Live tip. On relay loss it re-attaches (same hop first, then the others; re-announces when the
 * hop changes). End announces Ended, clears the key, detaches and stops capture. No call objects.
 *
 * Threading: passive, on its runner's owner (THREADING.md § Owner runners). Results come back as
 * `BroadcasterEvent`s through its outbox, carrying the show they belong to; one for an older show
 * (End / a newer GoLive since) is dropped. Frames are sealed and sent on the engine's capture thread.
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
    /** The video level being published; 0 = audio only. */
    int video_level = 0;
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
  /** Frames sent this show; readable from any thread for the workflow's lifetime. */
  std::shared_ptr<const std::atomic<uint64_t>> FramesSentCounter() const { return frames_sent_; }
  void SetOnStatusChanged(std::function<void()> callback) { on_status_changed_ = std::move(callback); }
  /** Where its events go (the runner hands them back to `Handle` on the owner). */
  void SetOutbox(OwnerOutbox<BroadcasterEvent> outbox) { outbox_ = std::move(outbox); }
  void Handle(BroadcasterEvent& event);

  static const char* PhaseName(Phase phase);

private:
  struct Sender;

  void SetPhase(Phase phase, std::string hop = {});
  void Fail(const std::string& error);
  void Teardown(bool announce_end);
  void AttachNext(const std::string& why);
  void OnAttached(const std::string& hop, Roe<MediaRelayAttached> attached);
  void StartPublishing(const std::string& hop);
  void Announce(PeerAnnounceState state, std::function<void(Roe<void>)> on_done);
  void OnSessionEnded(MediaRelayClientLoss loss);
  void OnAnnounced(bool first, Roe<void> announced);
  /** Adopt the level the relay answered (the highest: one encoder); reopen the camera if it changed. */
  void ApplyVideoLevels(const std::vector<uint8_t>& carried);
  /** A new show (or none): results of the previous one are stale from here on. */
  void NewShow();

  BroadcasterPorts ports_;
  Status status_;
  std::function<void()> on_status_changed_;
  OwnerOutbox<BroadcasterEvent> outbox_;
  uint64_t show_ = 0;
  /** `show_` for I/O-side checks (attach still wanted). */
  std::shared_ptr<std::atomic<uint64_t>> current_show_ = std::make_shared<std::atomic<uint64_t>>(0);
  OwnerExecutor::TimerId reattach_timer_ = 0;
  std::shared_ptr<std::atomic<uint64_t>> frames_sent_ = std::make_shared<std::atomic<uint64_t>>(0);

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
  bool video_wanted_ = false;
  /** B009: the level published now (0 = audio only), and the one the Live tip announced. */
  uint8_t video_level_ = 0;
  uint8_t announced_video_level_ = 0;
  uint64_t lost_observer_ = 0;
  int consecutive_losses_ = 0;
};

} // namespace pbr
