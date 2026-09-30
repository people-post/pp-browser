#include "domain/mesh/l4/call_media/CallMediaLegCoordinator.h"
#include "common/media/MediaChannel.h"

#include "domain/mesh/l4/shared/ChannelSessionSlot.h"
#include "domain/mesh/l4/shared/ProductChannelPolicies.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/PeerLink.h"
#include "domain/mesh/l4/call_media/CallMediaBundleLogic.h"
#include "domain/mesh/l4/call_media/CallMediaFrameCrypto.h"
#include "domain/mesh/l4/call_media/CallMediaSessionLogic.h"
#include "common/LengthPrefixedCodec.h"
#include "common/Logger.h"

#include "common/ValueJson.h"
#include "foundation/runtime/DeferredSelf.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"
#include "domain/mesh/shared/AmpChannelOpen.h"

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

/** Dogfood filter matches CallLifecycle|StartSfu — keep that token in messages. */
logging::Logger& CallMediaLegLog() {
  static logging::Logger log = logging::getLogger("CallMediaLeg");
  return log;
}

const char* BundlePhaseName(const CallMediaBundlePhase phase) {
  switch (phase) {
  case CallMediaBundlePhase::Idle:
    return "Idle";
  case CallMediaBundlePhase::OutboundHello:
    return "OutboundHello";
  case CallMediaBundlePhase::InboundHello:
    return "InboundHello";
  case CallMediaBundlePhase::AwaitingMedia:
    return "AwaitingMedia";
  case CallMediaBundlePhase::MediaReady:
    return "MediaReady";
  case CallMediaBundlePhase::Closing:
    return "Closing";
  }
  return "?";
}

std::vector<uint8_t> Utf8Body(const std::string& utf8) {
  return std::vector<uint8_t>(utf8.begin(), utf8.end());
}

/** k3: a candidate path must be up and acknowledged within this, or the call stays where it is. */
constexpr auto kMigrateTimeout = std::chrono::seconds(5);
/** k3: release the old path once media arrived on the new one and this long passed ... */
constexpr auto kRetireAfterSwitch = std::chrono::seconds(1);
/** ... or after this long regardless (a muted / silent peer). */
constexpr auto kRetireAtMost = std::chrono::seconds(5);
/** k3: between automatic attempts to move a relayed call onto a direct link. */
constexpr auto kAutoMigrateBackoff = std::chrono::seconds(10);
/** k4 (K008): a call without any path waits this long for a new one before it fails. */
constexpr auto kReconnectWindow = std::chrono::seconds(30);
/** k3: a retiring path whose release was never acknowledged is dropped after this. */
constexpr auto kRetireAbandon = std::chrono::seconds(10);

/** `migrate` moves the call onto the new path; `path_add` (k6) keeps it there as the standby. */
std::string BuildMigrateJson(const std::string& call_id, const uint32_t media_epoch, const uint32_t path_gen,
                             const bool standby_only = false) {
  Object o;
  o.set("v", static_cast<int64_t>(1));
  o.set("type", standby_only ? "path_add" : "migrate");
  o.set("call_id", call_id);
  o.set("media_epoch", static_cast<int64_t>(media_epoch));
  o.set("path_gen", static_cast<int64_t>(path_gen));
  return DumpJson(o);
}

std::string BuildMigrateAckJson(const bool ok, const uint32_t path_gen, const std::string& error = {}) {
  Object o;
  o.set("v", static_cast<int64_t>(1));
  o.set("type", "migrate_ack");
  o.set("ok", ok);
  o.set("path_gen", static_cast<int64_t>(path_gen));
  if (!error.empty()) {
    o.set("error", error);
  }
  return DumpJson(o);
}

/** k4 heartbeat; `active` marks the sender's active path (a peer seeing it on its standby follows). */
const std::string& HeartbeatJson(const bool active) {
  static const std::string on_active = R"({"v":1,"type":"hb","active":true})";
  static const std::string on_standby = R"({"v":1,"type":"hb"})";
  return active ? on_active : on_standby;
}

std::string BuildPathReleaseJson(const char* type, const uint32_t path_gen) {
  Object o;
  o.set("v", static_cast<int64_t>(1));
  o.set("type", type);
  o.set("path_gen", static_cast<int64_t>(path_gen));
  return DumpJson(o);
}

Roe<Object> ParseJsonObject(const std::string& json_utf8) {
  auto root = TryParseObject(json_utf8);
  if (!root) {
    return Error("invalid call-media json");
  }
  return *root;
}

std::string BuildHelloJson(const CallMediaDirectConnectParams& params) {
  Object hello;
  hello.set("v", int64_t{1});
  hello.set("type", "hello");
  hello.set("call_id", params.call_id);
  hello.setJsonUInt("media_epoch", params.media_epoch);
  hello.set("role", params.offerer ? "offerer" : "answerer");
  return DumpJson(hello);
}

std::string BuildHelloAckJson(const bool ok, const char* error = nullptr) {
  Object ack;
  ack.set("v", int64_t{1});
  ack.set("type", "hello_ack");
  ack.set("ok", ok);
  if (!ok && error) {
    ack.set("error", error);
  }
  return DumpJson(ack);
}

bool IsRemoteTerminalReason(const char* reason) {
  return reason && (std::strcmp(reason, "peer_close") == 0 || std::strcmp(reason, "peer_reset") == 0);
}

bool IsPendingCallId(const std::string& call_id) {
  return call_id.rfind("__pending_", 0) == 0;
}

} // namespace

struct CallMediaLegCoordinator::Impl : std::enable_shared_from_this<Impl> {
  /**
   * One path of a call (call-path-resilience k3): the link its channels are bound on and those
   * channels. The call's identity, key and callbacks stay on the Bundle across paths.
   */
  struct Path {
    /** The link the channels are bound on (set with the first channel). */
    pp::amp::LinkHandle link{};
    pp::amp::ChannelMux* mux = nullptr;
    CallMediaLinkKind kind = CallMediaLinkKind::Unknown;
    /** Path generation within the call (0 = the path the call started on). */
    uint32_t gen = 0;
    std::shared_ptr<pp::amp::ChannelSession> outbound_control;
    std::shared_ptr<pp::amp::ChannelSession> inbound_control;
    std::shared_ptr<pp::amp::ChannelSession> media;
    bool media_bound = false;
    /** k4: last heartbeat / control / media received on this path, and our last heartbeat on it. */
    Clock::time_point last_rx{};
    Clock::time_point last_hb_tx{};
  };

  struct Bundle {
    CallMediaLegId leg_id{};
    std::string call_id;
    CallMediaBundlePhase phase = CallMediaBundlePhase::Idle;
    bool offerer = false;
    /** Role from the remote's hello (glare); unset until one arrives. */
    std::optional<bool> remote_offerer;
    bool local_cancel = false;
    bool finished = true;
    bool control_ready = false;

    CallMediaDirectConnectParams params;
    CallMediaDirectCallbacks callbacks;
    LegFinished on_finished;
    Clock::time_point deadline{};
    /**
     * Authenticated remote PeerId and mux of the link our channels are bound on. The dial alias in
     * params.peer_key can move (EnsureAssociation rebinds a Connected link from `account:` to the
     * PeerId key — one alias per link), so lookups fall back to the remote PeerId and verify the
     * mux is still the one we bound to.
     */
    std::string remote_peer_id;
    /** The path media flows on (k3: candidate / retiring paths join it during a migration). */
    Path active;
    /** k3: the candidate path being brought up (make-before-break). */
    std::optional<Path> candidate;
    /** k3: the previous path after a switch, drained until `path_release` is acknowledged. */
    std::optional<Path> retiring;
    /** k4: a warm fallback path (bound channels, slow heartbeat); failover switches TX to it. */
    std::optional<Path> standby;
    /** k4: the peer sends heartbeats (seen one this call) — only then does silence mean a dead path. */
    bool peer_heartbeats = false;
    Clock::time_point failed_over_at{};
    /** k4: set while the call has no path (lost the last one); a migration clears it. */
    Clock::time_point reconnect_deadline{};
    bool Reconnecting() const { return reconnect_deadline.time_since_epoch().count() != 0; }
    /**
     * Held `on_path_lost` while a quiet rebind runs (the peer was still connected on another link):
     * raised here unless a migration brings the call back first.
     */
    Clock::time_point path_lost_notify_at{};

    struct Migration {
      /** This side drives it (glare winner): opens the channels, sends migrate / path_release. */
      bool initiator = false;
      Clock::time_point deadline{};
      /** Initiator: channel opened on the candidate link, waiting for Open (polled on the IO tick). */
      uint32_t pending_control = 0;
      uint32_t pending_media = 0;
      LegFinished done;
      /** k6 `path_add`: the new path becomes the standby; the call stays on its active path. */
      bool standby_only = false;
    };
    std::optional<Migration> migration;
    /** Set at a switch; the driver releases `retiring` from here. */
    bool drove_switch = false;
    Clock::time_point switched_at{};
    bool rx_since_switch = false;
    bool release_sent = false;
    Clock::time_point release_sent_at{};
    /** The direct link this call left for the relay; never auto-migrated back onto. */
    pp::amp::LinkHandle left_direct{};
    /** Earliest next automatic relayed → direct attempt (backoff after one). */
    Clock::time_point next_auto_migrate{};
    /** Transport-side seq de-dupe per media channel: overlapping paths can deliver a frame twice. */
    std::unordered_map<uint8_t, CallMediaSeqWindow> rx_seq;
  };

  pp::amp::MeshRuntime* runtime = nullptr;
  mutable std::mutex mu;
  /**
   * User callbacks (on_connected / on_failed / on_finished) captured while `mu` is held.
   * They are invoked by CallbackLock only after `mu` is released: the transport/bridge
   * callbacks re-enter this coordinator (PrimaryLegId / IsActive), and `mu` is not
   * recursive, so invoking them under the lock deadlocked the Amp IO strand (B16).
   * Named `pending_user_cbs` (not `deferred`) so it does not collide with DeferredSelf `io_deferred`.
   */
  std::vector<std::function<void()>> pending_user_cbs;

  struct CallbackLock {
    explicit CallbackLock(Impl& impl) : impl_(impl), lock_(impl.mu) {}
    ~CallbackLock() { unlock(); }
    void unlock() {
      if (!lock_.owns_lock()) {
        return;
      }
      std::vector<std::function<void()>> pending;
      pending.swap(impl_.pending_user_cbs);
      lock_.unlock();
      for (auto& fn : pending) {
        fn();
      }
    }
    Impl& impl_;
    std::unique_lock<std::mutex> lock_;
  };
  InboundHandler inbound;
  /** k3 test seams: a peer from before migration (ignores `migrate`), and a short migrate timeout. */
  std::atomic<bool> ignore_migrate_for_test{false};
  /** k4 test seam: this side sends nothing (media, heartbeats) on paths of this kind — a path gone quiet. */
  std::atomic<CallMediaLinkKind> silenced_kind_for_test{CallMediaLinkKind::Unknown};
  std::chrono::milliseconds reconnect_window = std::chrono::duration_cast<std::chrono::milliseconds>(kReconnectWindow);
  /** k3: the driver moves a relayed call onto a direct link to the same peer as soon as one is up. */
  std::atomic<bool> auto_migrate_to_direct{true};
  std::chrono::milliseconds migrate_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(kMigrateTimeout);
  std::atomic<bool> stopped{false};
  std::atomic<bool> started{false};
  std::atomic<uint64_t> next_leg_id{1};
  pp::amp::MeshRuntime::IoTickId io_tick_id = 0;
  /** Guards PostIo(raw this) past Stop — OWNERSHIP.md § DeferredSelf.
   * IoTick / protocol use weak_ptr(Impl) instead of a separate lifetime ticket. */
  DeferredSelf io_deferred;

  /** call_id → bundle */
  std::unordered_map<std::string, std::unique_ptr<Bundle>> bundles;
  /** channel_id → (call_id, role) for close/frame ownership checks */
  std::unordered_map<uint32_t, std::pair<std::string, CallMediaChannelRole>> channel_index;

  void PostIo(std::function<void()> task) {
    if (!runtime || stopped.load(std::memory_order_acquire) || !task) {
      return;
    }
    io_deferred.Post([rt = runtime](std::function<void()> t) {
      if (rt) {
        rt->PostToIo(std::move(t));
      }
    }, std::move(task));
  }

  /** Remote PeerId for the glare tiebreak (dialed alias link first, then the given link). */
  std::string RemotePeerIdForGlare(const pp::amp::PeerLink& link) const {
    std::string remote = link.RemotePeerId();
    if (remote.empty() && !link.PeerKey().empty()) {
      if (const auto* known = runtime->Links().FindLink(link.PeerKey())) {
        remote = known->RemotePeerId();
      }
    }
    return remote;
  }

  /**
   * Antisymmetric glare winner for this bundle: offerer beats answerer; equal roles fall back to
   * the PeerId order. The remote role comes from its hello; until one arrived, assume the
   * complementary role (a normal call).
   */
  bool LocalWinsForBundle(const Bundle& bundle, const pp::amp::PeerLink& fallback_link) const {
    const pp::amp::PeerLink* link = &fallback_link;
    if (!bundle.params.peer_key.empty()) {
      if (const auto* dial = runtime->Links().FindLink(bundle.params.peer_key)) {
        link = dial;
      }
    }
    return LocalWinsCallMediaGlareForRoles(bundle.offerer, bundle.remote_offerer.value_or(!bundle.offerer),
                                           runtime->Links().LocalPeerId(), RemotePeerIdForGlare(*link));
  }

  /**
   * The link `path`'s channels are on: once bound, exactly that link by handle (null when it is
   * gone) — never whatever now holds the dial alias or PeerId, since a direct and a relayed link
   * to one peer coexist (A024). Unbound: the dial alias, then the remote PeerId.
   */
  pp::amp::PeerLink* ResolvePathLink(const Bundle& bundle, const Path& path) const {
    if (!runtime) {
      return nullptr;
    }
    if (path.link.valid()) {
      pp::amp::PeerLink* bound = nullptr;
      (void)runtime->Links().WithLiveLink(path.link, [&](pp::amp::PeerLink& live) { bound = &live; });
      return bound;
    }
    pp::amp::PeerLink* link = nullptr;
    if (!bundle.params.peer_key.empty()) {
      link = runtime->Links().FindLink(bundle.params.peer_key);
    }
    if (!link && !bundle.remote_peer_id.empty()) {
      link = runtime->Links().FindLinkByPeerId(bundle.remote_peer_id);
    }
    return link;
  }

  pp::amp::PeerLink* ResolveLink(const Bundle& bundle) const { return ResolvePathLink(bundle, bundle.active); }

  static void NotePathLink(Path& path, pp::amp::PeerLink& link) {
    if (!path.mux) {
      path.mux = link.Mux();
      path.link = link.Handle();
      path.kind = link.IsCarrierBacked() ? CallMediaLinkKind::Relayed : CallMediaLinkKind::Direct;
    }
  }

  void NoteBoundLink(Bundle& bundle, Path& path, pp::amp::PeerLink& link) {
    if (bundle.params.peer_key.empty()) {
      bundle.params.peer_key = link.PeerKey();
    }
    if (bundle.remote_peer_id.empty()) {
      bundle.remote_peer_id = link.RemotePeerId();
    }
    NotePathLink(path, link);
  }

  bool BundleMatchesLink(const Bundle& bundle, const pp::amp::PeerLink& link) const {
    if (!bundle.params.peer_key.empty() && bundle.params.peer_key == link.PeerKey()) {
      return true;
    }
    return ResolveLink(bundle) == &link;
  }

  bool OtherBundleBusy(const std::string& except_call_id) const {
    for (const auto& [id, bundle] : bundles) {
      if (id == except_call_id || !bundle || IsPendingCallId(id)) {
        continue;
      }
      if (bundle->phase == CallMediaBundlePhase::MediaReady ||
          bundle->phase == CallMediaBundlePhase::AwaitingMedia ||
          bundle->phase == CallMediaBundlePhase::InboundHello) {
        return true;
      }
    }
    return false;
  }

  Bundle* FindByCallId(const std::string& call_id) {
    const auto it = bundles.find(call_id);
    return it == bundles.end() ? nullptr : it->second.get();
  }

  Bundle* FindByLegId(const CallMediaLegId id) {
    if (!id) {
      return nullptr;
    }
    for (auto& [_, bundle] : bundles) {
      if (bundle && bundle->leg_id.value == id.value) {
        return bundle.get();
      }
    }
    return nullptr;
  }

  Bundle* PrimaryBundle() {
    Bundle* ready = nullptr;
    Bundle* active = nullptr;
    for (auto& [id, bundle] : bundles) {
      if (!bundle || IsPendingCallId(id) || bundle->phase == CallMediaBundlePhase::Idle) {
        continue;
      }
      if (bundle->phase == CallMediaBundlePhase::MediaReady) {
        ready = bundle.get();
      } else if (!active) {
        active = bundle.get();
      }
    }
    return ready ? ready : active;
  }

  const Bundle* PrimaryBundle() const {
    return const_cast<Impl*>(this)->PrimaryBundle();
  }

  void IndexChannel(const uint32_t channel_id, const std::string& call_id, const CallMediaChannelRole role) {
    channel_index[channel_id] = {call_id, role};
  }

  void UnindexChannel(const uint32_t channel_id) {
    channel_index.erase(channel_id);
  }

  static bool PathOwnsRole(const Path& path, const CallMediaChannelRole role, const pp::amp::ChannelSession* session) {
    if (!session) {
      return false;
    }
    switch (role) {
    case CallMediaChannelRole::OutboundControl:
      return path.outbound_control.get() == session;
    case CallMediaChannelRole::InboundControl:
      return path.inbound_control.get() == session;
    case CallMediaChannelRole::Media:
      return path.media.get() == session;
    }
    return false;
  }

  bool OwnsRole(const Bundle& bundle, const CallMediaChannelRole role,
                const pp::amp::ChannelSession* session) const {
    return PathOwnsRole(bundle.active, role, session);
  }

  /** True when CloseQuiet/ReleaseHandlers may safely touch the path's mux. */
  bool MuxAliveForPath(const Bundle& bundle, const Path& path) const {
    pp::amp::PeerLink* link = ResolvePathLink(bundle, path);
    return link && link->Mux() && link->Phase() == pp::amp::PeerLinkPhase::Connected;
  }

  bool MuxAliveForBundle(const Bundle& bundle) const { return MuxAliveForPath(bundle, bundle.active); }

  /** The path's PeerLink was erased (DropLink) — its ChannelSessions' mux may already be dangling. */
  bool PathLinkMissing(const Bundle& bundle, const Path& path) const {
    if (!runtime || (!path.link.valid() && bundle.params.peer_key.empty() && bundle.remote_peer_id.empty())) {
      return false;
    }
    return ResolvePathLink(bundle, path) == nullptr;
  }

  bool PeerLinkMissing(const Bundle& bundle) const { return PathLinkMissing(bundle, bundle.active); }

  void DropPathRole(const Bundle& bundle, Path& path, const CallMediaChannelRole role) {
    pp::amp::PeerLink* link = ResolvePathLink(bundle, path);
    std::shared_ptr<pp::amp::ChannelSession>* slot = nullptr;
    switch (role) {
    case CallMediaChannelRole::OutboundControl:
      slot = &path.outbound_control;
      break;
    case CallMediaChannelRole::InboundControl:
      slot = &path.inbound_control;
      break;
    case CallMediaChannelRole::Media:
      slot = &path.media;
      path.media_bound = false;
      break;
    }
    if (*slot) {
      UnindexChannel((*slot)->ChannelId());
    }
    CloseQuietSlot(*slot, link);
  }

  void DropRole(Bundle& bundle, const CallMediaChannelRole role) { DropPathRole(bundle, bundle.active, role); }

  void DropPath(const Bundle& bundle, Path& path) {
    DropPathRole(bundle, path, CallMediaChannelRole::OutboundControl);
    DropPathRole(bundle, path, CallMediaChannelRole::InboundControl);
    DropPathRole(bundle, path, CallMediaChannelRole::Media);
  }

  /** Which of the bundle's paths holds `session` in `role` (null: none). */
  Path* PathOwning(Bundle& bundle, const CallMediaChannelRole role, const pp::amp::ChannelSession* session) {
    if (PathOwnsRole(bundle.active, role, session)) {
      return &bundle.active;
    }
    if (bundle.candidate && PathOwnsRole(*bundle.candidate, role, session)) {
      return &*bundle.candidate;
    }
    if (bundle.retiring && PathOwnsRole(*bundle.retiring, role, session)) {
      return &*bundle.retiring;
    }
    if (bundle.standby && PathOwnsRole(*bundle.standby, role, session)) {
      return &*bundle.standby;
    }
    return nullptr;
  }

  /** The path of `bundle` whose media channel is `session` (null: none). */
  Path* PathOwningMedia(Bundle& bundle, const pp::amp::ChannelSession* session) {
    return session ? PathOwning(bundle, CallMediaChannelRole::Media, session) : nullptr;
  }

  /** The bundle + path whose control channel (either direction) is `session`. */
  std::pair<Bundle*, Path*> FindControlOwner(const pp::amp::ChannelSession* session) {
    for (auto& [_, bundle] : bundles) {
      if (!bundle) {
        continue;
      }
      for (const auto role : {CallMediaChannelRole::OutboundControl, CallMediaChannelRole::InboundControl}) {
        if (Path* path = PathOwning(*bundle, role, session)) {
          return {bundle.get(), path};
        }
      }
    }
    return {nullptr, nullptr};
  }

  void FinishBundle(Bundle& bundle, Roe<void> result) {
    if (bundle.finished) {
      return;
    }
    bundle.finished = true;
    LegFinished cb = std::move(bundle.on_finished);
    bundle.on_finished = {};
    if (cb) {
      pending_user_cbs.push_back([cb = std::move(cb), result = std::move(result)]() mutable { cb(std::move(result)); });
    }
  }

  void EraseBundle(const std::string& call_id) {
    auto* bundle = FindByCallId(call_id);
    if (!bundle) {
      return;
    }
    DropPath(*bundle, bundle->active);
    if (bundle->candidate) {
      DropPath(*bundle, *bundle->candidate);
    }
    if (bundle->retiring) {
      DropPath(*bundle, *bundle->retiring);
    }
    if (bundle->standby) {
      DropPath(*bundle, *bundle->standby);
    }
    if (bundle->migration && bundle->migration->done) {
      pending_user_cbs.push_back([done = std::move(bundle->migration->done)]() { done(Error("call-media: call ended")); });
    }
    bundles.erase(call_id);
  }

  void TearDownBundle(Bundle& bundle, const bool finish_with_abort, const bool notify_failed,
                      const std::string& fail_message) {
    const std::string call_id = bundle.call_id;
    CallMediaLegLog().info << "CallMediaLeg teardown call_id=" << call_id << " peer=" << bundle.params.peer_key
                           << " phase=" << BundlePhaseName(bundle.phase)
                           << " abort=" << (finish_with_abort ? 1 : 0) << " notify=" << (notify_failed ? 1 : 0)
                           << " reason=" << fail_message;
    bundle.local_cancel = bundle.local_cancel || finish_with_abort;
    bundle.phase = CallMediaBundlePhase::Closing;
    if (!bundle.finished) {
      if (finish_with_abort) {
        FinishBundle(bundle, Error("call-media aborted"));
      } else if (notify_failed) {
        if (bundle.callbacks.on_failed && !fail_message.empty()) {
          pending_user_cbs.push_back([on_failed = bundle.callbacks.on_failed, fail_message]() { on_failed(fail_message); });
        }
        FinishBundle(bundle, Error(fail_message.empty() ? "call-media failed" : fail_message));
      } else {
        FinishBundle(bundle, Error(fail_message.empty() ? "call-media stream closed" : fail_message));
      }
    }
    EraseBundle(call_id);
  }

  void EnterMediaReady(Bundle& bundle) {
    if (bundle.phase == CallMediaBundlePhase::MediaReady) {
      return;
    }
    if (!bundle.control_ready || !bundle.active.media_bound) {
      return;
    }
    bundle.phase = CallMediaBundlePhase::MediaReady;
    bundle.active.last_rx = Clock::now();
    if (bundle.callbacks.on_connected) {
      pending_user_cbs.push_back(bundle.callbacks.on_connected);
    }
    FinishBundle(bundle, {});
  }

  void TryEnterMediaReady(Bundle& bundle) { EnterMediaReady(bundle); }

  /** Waits on the channel's own link (by handle): a direct and a relayed link to one peer coexist. */
  void ScheduleWhenChannelOpen(const pp::amp::LinkHandle link, const uint32_t channel_id,
                               const Clock::time_point deadline, std::function<void(bool open)> done) {
    if (!runtime || !link.valid()) {
      done(false);
      return;
    }
    AmpWhenChannelOpenOnLink(runtime->Links(), link, channel_id, deadline, std::move(done));
  }

  pp::amp::PeerLink* LiveLink(const pp::amp::LinkHandle handle) const {
    pp::amp::PeerLink* link = nullptr;
    if (runtime && handle.valid()) {
      (void)runtime->Links().WithLiveLink(handle, [&](pp::amp::PeerLink& live) { link = &live; });
    }
    return link;
  }

  void TickDeadlines() {
    const auto now = Clock::now();
    std::vector<std::string> timed_out;
    std::vector<std::string> link_lost;
    std::vector<std::string> reconnect_expired;
    {
      CallbackLock lock(*this);
      for (auto& [call_id, bundle] : bundles) {
        if (!bundle || IsPendingCallId(call_id)) {
          continue;
        }
        if (bundle->phase == CallMediaBundlePhase::Idle || bundle->phase == CallMediaBundlePhase::Closing) {
          continue;
        }
        // k3/k4 paths first: a lost active link fails over to the standby, or waits for a new
        // path (reconnect window), before it counts as lost.
        if (bundle->phase == CallMediaBundlePhase::MediaReady) {
          TickPaths(*bundle, now);
          if (bundle->Reconnecting()) {
            if (bundle->path_lost_notify_at.time_since_epoch().count() != 0 &&
                now >= bundle->path_lost_notify_at) {
              CallMediaLegLog().info << "CallMediaLeg quiet rebind did not land call_id=" << call_id;
              NotifyPathLost(*bundle);
            }
            if (now >= bundle->reconnect_deadline) {
              reconnect_expired.push_back(call_id);
            }
            continue;
          }
        }
        // PeerLink drop leaves ChannelSession mux_ dangling — tear down before L4 touches it.
        // Do not treat Handshaking as lost (FindLink still present until DropLink).
        // OutboundHello may not have FindLink yet while EnsureAssociation starts — prefer the
        // connect deadline over peer-link-lost (avoids racing TearDown vs dial).
        if (PeerLinkMissing(*bundle)) {
          const bool outbound_dialing =
              bundle->phase == CallMediaBundlePhase::OutboundHello &&
              runtime->Links().GetLinkSnapshot(bundle->params.peer_key).has_endpoint;
          if (!outbound_dialing) {
            link_lost.push_back(call_id);
            continue;
          }
        }
        if (bundle->finished) {
          continue;
        }
        if (bundle->deadline.time_since_epoch().count() == 0) {
          continue;
        }
        if (now >= bundle->deadline && bundle->phase != CallMediaBundlePhase::MediaReady) {
          timed_out.push_back(call_id);
        }
      }
    }
    for (const auto& call_id : reconnect_expired) {
      CallbackLock lock(*this);
      if (auto* bundle = FindByCallId(call_id); bundle && bundle->Reconnecting()) {
        const std::string why = "amp call-media: no path within the reconnect window";
        CallMediaLegLog().warning << "CallMediaLeg reconnect window expired call_id=" << call_id;
        // A MediaReady leg has finished, so TearDownBundle reports nothing: report the loss here.
        if (bundle->callbacks.on_failed) {
          pending_user_cbs.push_back([on_failed = bundle->callbacks.on_failed, why]() { on_failed(why); });
        }
        TearDownBundle(*bundle, /*finish_with_abort=*/false, /*notify_failed=*/true, why);
      }
    }
    for (const auto& call_id : link_lost) {
      CallbackLock lock(*this);
      auto* bundle = FindByCallId(call_id);
      if (!bundle || bundle->phase == CallMediaBundlePhase::Idle ||
          bundle->phase == CallMediaBundlePhase::Closing) {
        continue;
      }
      TearDownBundle(*bundle, /*finish_with_abort=*/false, /*notify_failed=*/true,
                     "amp call-media: peer link lost");
    }
    for (const auto& call_id : timed_out) {
      CallbackLock lock(*this);
      auto* bundle = FindByCallId(call_id);
      if (!bundle || bundle->finished) {
        continue;
      }
      CallMediaLegLog().info << "CallMediaLeg timeout call_id=" << call_id
                             << " peer=" << bundle->params.peer_key
                             << " phase=" << BundlePhaseName(bundle->phase)
                             << " role=" << (bundle->offerer ? "offerer" : "answerer");
      TearDownBundle(*bundle, /*finish_with_abort=*/false, /*notify_failed=*/false,
                     "amp call-media connect timed out");
    }
  }

  /**
   * A channel's close callback runs on whichever thread drove it: a media send that hits a dead
   * channel closes it on the sender's thread. Close handling takes `mu` and then the link manager's
   * lock — the IO pump takes them the other way round — so it always runs on the IO strand.
   */
  void PostChannelClosed(const std::string& call_id, const CallMediaChannelRole role,
                         const std::shared_ptr<pp::amp::ChannelSession>& session, const char* reason) {
    PostIo([this, self = shared_from_this(), call_id, role, session, why = std::string(reason ? reason : "")]() {
      OnChannelClosed(call_id, role, session, why.c_str());
    });
  }

  void OnChannelClosed(const std::string& /*call_id*/, const CallMediaChannelRole role,
                       const std::shared_ptr<pp::amp::ChannelSession>& session, const char* reason) {
    CallbackLock lock(*this);
    Bundle* bundle = nullptr;
    Path* path = nullptr;
    for (auto& [_, b] : bundles) {
      if (b) {
        if (Path* owning = PathOwning(*b, role, session.get())) {
          bundle = b.get();
          path = owning;
          break;
        }
      }
    }
    if (!bundle) {
      return;
    }
    if (bundle->candidate && path == &*bundle->candidate) {
      AbandonMigrationLinkLost(*bundle, std::string("candidate path channel closed (") + (reason ? reason : "") + ")");
      return;
    }
    if (bundle->retiring && path == &*bundle->retiring) {
      // An old path closing after (or while) it is released is expected, never a failure.
      DropPath(*bundle, *bundle->retiring);
      bundle->retiring.reset();
      return;
    }
    if (bundle->standby && path == &*bundle->standby) {
      CallMediaLegLog().info << "CallMediaLeg standby path closed call_id=" << bundle->call_id
                             << " reason=" << (reason ? reason : "");
      DropPath(*bundle, *bundle->standby);
      bundle->standby.reset();
      return;
    }
    // The path the call just moved onto died while the one it left is still draining: go back to it.
    if (path == &bundle->active && bundle->phase == CallMediaBundlePhase::MediaReady && !bundle->local_cancel &&
        !IsRemoteTerminalReason(reason) && bundle->retiring && !PathLinkMissing(*bundle, *bundle->retiring)) {
      FallBackToRetiring(*bundle, std::string("active path closed (") + (reason ? reason : "") + ")");
      return;
    }
    // k4: the active path's link went away under the call (not the peer hanging up): take the standby.
    if (path == &bundle->active && bundle->phase == CallMediaBundlePhase::MediaReady && !bundle->local_cancel &&
        !IsRemoteTerminalReason(reason) && bundle->standby && !PathLinkMissing(*bundle, *bundle->standby)) {
      FailOverToStandby(*bundle, std::string("active path closed (") + (reason ? reason : "") + ")");
      return;
    }
    if (path == &bundle->active && bundle->phase == CallMediaBundlePhase::MediaReady && !bundle->local_cancel &&
        !IsRemoteTerminalReason(reason)) {
      EnterPathLost(*bundle, std::string("active path closed (") + (reason ? reason : "") + ")");
      return;
    }
    CallMediaChannelCloseContext ctx;
    ctx.phase = bundle->phase;
    ctx.role = role;
    ctx.local_cancel = bundle->local_cancel;
    ctx.remote_terminal = IsRemoteTerminalReason(reason);
    ctx.slot_still_owned = true;
    const auto decision = DecideCallMediaChannelClose(ctx);
    if (decision == CallMediaChannelCloseDecision::Ignore) {
      // Clear a dead inbound slot without failing the outbound winner.
      if (role == CallMediaChannelRole::InboundControl &&
          bundle->phase == CallMediaBundlePhase::OutboundHello) {
        DropRole(*bundle, CallMediaChannelRole::InboundControl);
      }
      return;
    }
    const bool notify = decision == CallMediaChannelCloseDecision::FailLeg && ctx.remote_terminal;
    const std::string message =
        std::string("call-media stream closed (") + (reason && reason[0] ? reason : "unknown") + ")";
    TearDownBundle(*bundle, /*finish_with_abort=*/false, notify, message);
  }

  void OpenMediaOutbound(Bundle& bundle) {
    pp::amp::PeerLink* link = ResolveLink(bundle);
    if (!link || !link->Mux()) {
      TearDownBundle(bundle, false, false, "amp call-media: no link for media channel");
      return;
    }
    auto channel_id = link->Mux()->OpenOutbound(kCallMediaDirectProtocolId, pp::amp::CallMediaChannelPolicy(std::chrono::milliseconds{0}));
    if (!channel_id) {
      TearDownBundle(bundle, false, false, channel_id.error().message);
      return;
    }
    const auto call_id = bundle.call_id;
    const auto leg_id = bundle.leg_id;
    const auto deadline = bundle.deadline;
    ScheduleWhenChannelOpen(link->Handle(), *channel_id, deadline,
                            [this, self = shared_from_this(), call_id, leg_id,
                             channel_id = *channel_id](const bool open) {
                              CallbackLock lock(*this);
                              auto* bundle = FindByCallId(call_id);
                              if (!bundle || bundle->leg_id.value != leg_id.value) {
                                return;
                              }
                              if (!open) {
                                if (bundle->phase == CallMediaBundlePhase::OutboundHello ||
                                    bundle->phase == CallMediaBundlePhase::AwaitingMedia) {
                                  TearDownBundle(*bundle, false, false,
                                                 "amp call-media: media channel open failed");
                                }
                                return;
                              }
                              auto* resolved = ResolveLink(*bundle);
                              if (!resolved) {
                                TearDownBundle(*bundle, false, false, "amp call-media: peer link missing");
                                return;
                              }
                              BindMediaChannel(*bundle, *resolved, channel_id);
                              TryEnterMediaReady(*bundle);
                            });
  }

  void BindControlChannel(Bundle& bundle, pp::amp::PeerLink& link, const uint32_t channel_id,
                          const CallMediaChannelRole role) {
    BindControlChannel(bundle, bundle.active, link, channel_id, role);
  }

  void BindControlChannel(Bundle& bundle, Path& path, pp::amp::PeerLink& link, const uint32_t channel_id,
                          const CallMediaChannelRole role) {
    NoteBoundLink(bundle, path, link);
    auto channel_session = std::make_shared<pp::amp::ChannelSession>();
    const std::string call_id = bundle.call_id;
    channel_session->Bind(
        *link.Mux(), channel_id, pp::amp::CallMediaControlChannelPolicy(),
        [this, self = shared_from_this(), call_id, role, channel_session](Roe<std::vector<uint8_t>> frame) {
          if (!frame) {
            return false;
          }
          const std::string json_utf8(frame->begin(), frame->end());
          HandleControlJson(call_id, role, channel_session, json_utf8);
          return true;
        },
        [this, self = shared_from_this(), call_id, role, channel_session](const char* reason) {
          PostChannelClosed(call_id, role, channel_session, reason);
        });
    IndexChannel(channel_id, call_id, role);
    if (role == CallMediaChannelRole::OutboundControl) {
      path.outbound_control = std::move(channel_session);
    } else {
      path.inbound_control = std::move(channel_session);
    }
  }

  void BindMediaChannel(Bundle& bundle, pp::amp::PeerLink& link, const uint32_t channel_id) {
    BindMediaChannel(bundle, bundle.active, link, channel_id);
  }

  void BindMediaChannel(Bundle& bundle, Path& path, pp::amp::PeerLink& link, const uint32_t channel_id) {
    NoteBoundLink(bundle, path, link);
    auto channel_session = std::make_shared<pp::amp::ChannelSession>();
    const std::string call_id = bundle.call_id;
    std::weak_ptr<pp::amp::ChannelSession> weak_session = channel_session;
    channel_session->Bind(
        *link.Mux(), channel_id, pp::amp::CallMediaChannelPolicy(std::chrono::milliseconds{0}),
        [this, self = shared_from_this(), call_id, weak_session](Roe<std::vector<uint8_t>> frame) {
          if (!frame) {
            return false;
          }
          return HandleMediaBody(call_id, weak_session.lock().get(), *frame);
        },
        [this, self = shared_from_this(), call_id, channel_session](const char* reason) {
          PostChannelClosed(call_id, CallMediaChannelRole::Media, channel_session, reason);
        });
    IndexChannel(channel_id, call_id, CallMediaChannelRole::Media);
    path.media = std::move(channel_session);
    path.media_bound = true;
  }

  bool HandleMediaBody(const std::string& call_id, const pp::amp::ChannelSession* from,
                       const std::vector<uint8_t>& frame) {
    CallMediaDirectCallbacks cbs;
    CallMediaDirectConnectParams params;
    {
      CallbackLock lock(*this);
      auto* bundle = FindByCallId(call_id);
      if (!bundle || bundle->phase != CallMediaBundlePhase::MediaReady) {
        return true;
      }
      cbs = bundle->callbacks;
      params = bundle->params;
    }
    if (frame.size() < 8) {
      return true;
    }
    const uint64_t len = DecodeLengthPrefixedHeader(std::vector<uint8_t>(frame.begin(), frame.begin() + 8));
    if (len + 8 != frame.size()) {
      return true;
    }
    std::vector<uint8_t> body(frame.begin() + static_cast<std::ptrdiff_t>(8), frame.end());
    auto decoded = DecryptCallMediaFrame(params.media_key, params.call_id, params.media_epoch, body);
    if (!decoded) {
      return true;
    }
    {
      CallbackLock lock(*this);
      auto* bundle = FindByCallId(call_id);
      if (!bundle) {
        return true;
      }
      if (from && bundle->active.media.get() == from) {
        bundle->rx_since_switch = true;
      }
      if (Path* path = PathOwningMedia(*bundle, from)) {
        path->last_rx = Clock::now();
      }
      if (!bundle->rx_seq[decoded->channel].Accept(decoded->seq)) {
        return true;  // the same frame over the other path
      }
    }
    if (cbs.on_media) {
      cbs.on_media(decoded->channel, decoded->seq, decoded->mark, decoded->payload);
    } else if (decoded->channel == kMediaChannelAudio && cbs.on_audio) {
      cbs.on_audio(decoded->payload);
    }
    return true;
  }


  Bundle* FindByInboundSession(const std::shared_ptr<pp::amp::ChannelSession>& session) {
    for (auto& [_, bundle] : bundles) {
      if (bundle && bundle->active.inbound_control == session) {
        return bundle.get();
      }
    }
    return nullptr;
  }

  void RekeyPendingToCallId(Bundle& pending, const std::string& call_id) {
    if (pending.call_id == call_id) {
      return;
    }
    auto node = bundles.extract(pending.call_id);
    if (!node) {
      return;
    }
    node.key() = call_id;
    node.mapped()->call_id = call_id;
    if (node.mapped()->active.inbound_control) {
      IndexChannel(node.mapped()->active.inbound_control->ChannelId(), call_id, CallMediaChannelRole::InboundControl);
    }
    bundles.insert(std::move(node));
  }

  void HandleControlJson(const std::string& known_call_id, const CallMediaChannelRole role,
                         const std::shared_ptr<pp::amp::ChannelSession>& channel_session,
                         const std::string& json_utf8) {
    auto parsed = ParseJsonObject(json_utf8);
    if (!parsed) {
      return;
    }
    const auto type = parsed->getString("type").value_or("");
    {
      // k4: anything on a path's control channel proves the path alive; `hb` only does that.
      CallbackLock lock(*this);
      auto [bundle, path] = FindControlOwner(channel_session.get());
      if (path) {
        path->last_rx = Clock::now();
      }
      if (type == "hb") {
        if (bundle) {
          bundle->peer_heartbeats = true;
          // The peer's active-path heartbeat on our standby: it moved there — follow (both ends then
          // agree without waiting out their own silence timers).
          if (parsed->getIf<bool>("active").value_or(false) && bundle->standby && path == &*bundle->standby &&
              bundle->phase == CallMediaBundlePhase::MediaReady && !bundle->candidate && !bundle->retiring) {
            FailOverToStandby(*bundle, "peer moved to this path");
          }
        }
        return;
      }
    }
    if (type == "hello") {
      HandleInboundHello(channel_session, *parsed);
      return;
    }
    if (type == "hello_ack") {
      HandleHelloAck(known_call_id, role, *parsed);
      return;
    }
    // k3 path migration: routed by the channel, not the call id captured at bind (the responder's
    // candidate channel was bound as a placeholder).
    if (type == "migrate" || type == "path_add") {
      if (!ignore_migrate_for_test.load(std::memory_order_relaxed)) {
        HandleMigrate(channel_session, *parsed, /*standby_only=*/type == "path_add");
      }
    } else if (type == "migrate_ack") {
      HandleMigrateAck(channel_session.get(), *parsed);
    } else if (type == "path_release") {
      HandlePathRelease(channel_session, *parsed);
    } else if (type == "path_release_ack") {
      HandlePathReleaseAck(channel_session.get(), *parsed);
    }
  }

  // ---- k3 make-before-break migration --------------------------------------------------------

  /** Initiator: open a candidate path for `leg_id` on `target` (a Connected link to the peer). */
  void BeginMigration(const CallMediaLegId leg_id, const pp::amp::LinkHandle target, LegFinished done) {
    CallbackLock lock(*this);
    BeginMigrationLocked(leg_id, target, std::move(done));
  }

  void BeginMigrationLocked(const CallMediaLegId leg_id, const pp::amp::LinkHandle target, LegFinished done,
                            const bool standby_only = false) {
    const auto fail = [&](const std::string& why) {
      if (done) {
        pending_user_cbs.push_back([done = std::move(done), why]() { done(Error("call-media migrate: " + why)); });
      }
    };
    Bundle* bundle = FindByLegId(leg_id);
    if (!bundle || bundle->phase != CallMediaBundlePhase::MediaReady) {
      return fail("no live call");
    }
    if (bundle->candidate || bundle->retiring || bundle->migration) {
      return fail("migration in progress");
    }
    pp::amp::PeerLink* link = nullptr;
    (void)runtime->Links().WithLiveLink(target, [&](pp::amp::PeerLink& live) { link = &live; });
    if (!link || !link->Mux() || link->Phase() != pp::amp::PeerLinkPhase::Connected) {
      return fail("candidate link not connected");
    }
    if (link->Mux() == bundle->active.mux) {
      return fail("already on that link");
    }
    if (standby_only && bundle->standby) {
      return fail("the call already has a standby");
    }
    if (bundle->standby && link->Mux() == bundle->standby->mux) {
      DropPath(*bundle, *bundle->standby);  // fresh channels on that link replace the warm ones
      bundle->standby.reset();
    }
    auto channel = link->Mux()->OpenOutbound(kCallMediaDirectProtocolId, pp::amp::CallMediaControlChannelPolicy());
    if (!channel) {
      return fail(channel.error().message);
    }
    Path candidate;
    NotePathLink(candidate, *link);
    candidate.gen = bundle->active.gen + 1;
    bundle->candidate = std::move(candidate);
    Bundle::Migration migration;
    migration.initiator = true;
    migration.deadline = Clock::now() + migrate_timeout;
    migration.pending_control = *channel;
    migration.done = std::move(done);
    migration.standby_only = standby_only;
    bundle->migration = std::move(migration);
    CallMediaLegLog().info << "CallMediaLeg " << (standby_only ? "standby add" : "migrate") << " start call_id=" << bundle->call_id
                           << " path_gen=" << bundle->candidate->gen << " to="
                           << (bundle->candidate->kind == CallMediaLinkKind::Relayed ? "relayed" : "direct");
  }

  /**
   * The candidate's link went away (a punched link lost the dual-dial election): it cannot be tried
   * again, so the auto-migrate backoff must not keep the driver off the link that won (an unused
   * cold link lives only ~5 s).
   */
  void AbandonMigrationLinkLost(Bundle& bundle, const std::string& why) {
    bundle.next_auto_migrate = {};
    AbandonMigration(bundle, why);
  }

  /** Drop the candidate path; the call stays on its active path. */
  void AbandonMigration(Bundle& bundle, const std::string& why) {
    CallMediaLegLog().info << "CallMediaLeg migrate abandoned call_id=" << bundle.call_id << " reason=" << why;
    if (bundle.candidate) {
      DropPath(bundle, *bundle.candidate);
      bundle.candidate.reset();
    }
    if (bundle.migration) {
      if (bundle.migration->done) {
        pending_user_cbs.push_back(
            [done = std::move(bundle.migration->done), why]() { done(Error("call-media migrate: " + why)); });
      }
      bundle.migration.reset();
    }
  }

  /** The candidate's media flows: a migration switches onto it, a `path_add` keeps it as standby. */
  void CompleteCandidate(Bundle& bundle) {
    if (bundle.migration && bundle.migration->standby_only) {
      AdoptCandidateAsStandby(bundle);
    } else {
      SwitchToCandidate(bundle);
    }
  }

  /** k6 (K003): the added path becomes the call's warm standby; TX stays where it is. */
  void AdoptCandidateAsStandby(Bundle& bundle) {
    LegFinished done;
    if (bundle.migration) {
      done = std::move(bundle.migration->done);
      bundle.migration.reset();
    }
    Path added = std::move(*bundle.candidate);
    bundle.candidate.reset();
    added.last_rx = Clock::now();
    if (bundle.standby) {
      DropPath(bundle, *bundle.standby);
    }
    CallMediaLegLog().info << "CallMediaLeg standby added call_id=" << bundle.call_id << " path_gen=" << added.gen
                           << " path=" << (added.kind == CallMediaLinkKind::Relayed ? "relayed" : "direct");
    bundle.standby = std::move(added);
    if (done) {
      pending_user_cbs.push_back([done = std::move(done)]() { done(Roe<void>()); });
    }
  }

  /** Standby → active, active → retiring. TX follows at once; RX already accepts either path. */
  void SwitchToCandidate(Bundle& bundle) {
    const bool drove = bundle.migration && bundle.migration->initiator;
    LegFinished done;
    if (bundle.migration) {
      done = std::move(bundle.migration->done);
      bundle.migration.reset();
    }
    if (bundle.Reconnecting()) {
      // The active path is the lost one's empty placeholder: nothing to drain or release. Unbound,
      // it would even resolve to the new link by alias and pass for a live standby.
      CallMediaLegLog().info << "CallMediaLeg reconnected call_id=" << bundle.call_id
                             << (bundle.path_lost_notify_at.time_since_epoch().count() != 0 ? " (quiet rebind)" : "");
      bundle.reconnect_deadline = {};
      bundle.path_lost_notify_at = {};
      bundle.retiring.reset();
    } else {
      bundle.retiring = std::move(bundle.active);
    }
    bundle.active = std::move(*bundle.candidate);
    bundle.candidate.reset();
    bundle.active.last_rx = Clock::now();
    if (bundle.retiring && bundle.active.kind == CallMediaLinkKind::Relayed &&
        bundle.retiring->kind == CallMediaLinkKind::Direct) {
      bundle.left_direct = bundle.retiring->link;
    }
    bundle.drove_switch = drove;
    bundle.switched_at = Clock::now();
    bundle.rx_since_switch = false;
    bundle.release_sent = false;
    CallMediaLegLog().info << "CallMediaLeg migrate switched call_id=" << bundle.call_id
                           << " path_gen=" << bundle.active.gen << " path="
                           << (bundle.active.kind == CallMediaLinkKind::Relayed ? "relayed" : "direct")
                           << " driver=" << (drove ? 1 : 0);
    if (bundle.callbacks.on_path_changed) {
      pending_user_cbs.push_back([cb = bundle.callbacks.on_path_changed, kind = bundle.active.kind]() { cb(kind); });
    }
    if (done) {
      pending_user_cbs.push_back([done = std::move(done)]() { done(Roe<void>()); });
    }
  }

  /** Responder: a `migrate` on a new control channel (bound as a placeholder bundle). */
  void HandleMigrate(const std::shared_ptr<pp::amp::ChannelSession>& session, const Object& msg,
                     const bool standby_only) {
    CallbackLock lock(*this);
    Bundle* holder = FindByInboundSession(session);
    if (!holder || !IsPendingCallId(holder->call_id)) {
      return;
    }
    const std::string call_id = msg.getString("call_id").value_or("");
    const auto gen = static_cast<uint32_t>(msg.getNonNegInt("path_gen").value_or(0));
    const auto epoch = static_cast<uint32_t>(msg.getNonNegInt("media_epoch").value_or(0));
    const auto reject = [&](const char* why) {
      CallMediaLegLog().info << "CallMediaLeg migrate reject call_id=" << call_id << " reason=" << why;
      (void)session->EnqueueOutbound(Utf8Body(BuildMigrateAckJson(false, gen, why)));
      EraseBundle(holder->call_id);
    };
    Bundle* target = FindByCallId(call_id);
    if (!target || target->phase != CallMediaBundlePhase::MediaReady) {
      return reject("no live call");
    }
    if (epoch != target->params.media_epoch) {
      return reject("media_epoch");
    }
    if (target->retiring || (target->migration && !target->migration->initiator)) {
      return reject("busy");
    }
    if (gen != target->active.gen + 1) {
      return reject("path_gen");
    }
    pp::amp::PeerLink* link = ResolvePathLink(*holder, holder->active);
    if (!link) {
      return reject("link gone");
    }
    if (link->Mux() == target->active.mux) {
      return reject("same path");
    }
    if (standby_only && target->standby) {
      return reject("standby present");
    }
    if (target->migration && target->migration->initiator) {
      // Both ends started a migration at once: the glare winner's goes ahead (offerer, then PeerId).
      if (LocalWinsForBundle(*target, *link)) {
        return reject("busy");
      }
      AbandonMigration(*target, "yielded to the peer's migration");
    }
    Path candidate = std::move(holder->active);
    holder->active = Path{};
    EraseBundle(holder->call_id);  // the placeholder only: its channel now belongs to the call
    candidate.gen = gen;
    IndexChannel(candidate.inbound_control->ChannelId(), call_id, CallMediaChannelRole::InboundControl);
    target->candidate = std::move(candidate);
    Bundle::Migration migration;
    migration.initiator = false;
    migration.deadline = Clock::now() + kMigrateTimeout;
    migration.standby_only = standby_only;
    target->migration = std::move(migration);
    (void)session->EnqueueOutbound(Utf8Body(BuildMigrateAckJson(true, gen)));
    CallMediaLegLog().info << "CallMediaLeg migrate accept call_id=" << call_id << " path_gen=" << gen;
  }

  /** Initiator: the responder's answer on the candidate control channel. */
  void HandleMigrateAck(const pp::amp::ChannelSession* session, const Object& msg) {
    CallbackLock lock(*this);
    auto [bundle, path] = FindControlOwner(session);
    if (!bundle || !bundle->candidate || path != &*bundle->candidate || !bundle->migration ||
        !bundle->migration->initiator) {
      return;
    }
    if (!msg.getIf<bool>("ok").value_or(false)) {
      return AbandonMigration(*bundle, "peer refused (" + msg.getString("error").value_or("") + ")");
    }
    pp::amp::PeerLink* link = ResolvePathLink(*bundle, *bundle->candidate);
    if (!link || !link->Mux()) {
      return AbandonMigration(*bundle, "candidate link lost");
    }
    auto media = link->Mux()->OpenOutbound(kCallMediaDirectProtocolId,
                                          pp::amp::CallMediaChannelPolicy(std::chrono::milliseconds{0}));
    if (!media) {
      return AbandonMigration(*bundle, media.error().message);
    }
    bundle->migration->pending_media = *media;
  }

  /** Responder: the driver releases the old path. */
  void HandlePathRelease(const std::shared_ptr<pp::amp::ChannelSession>& session, const Object& msg) {
    CallbackLock lock(*this);
    auto [bundle, path] = FindControlOwner(session.get());
    if (!bundle || path != &bundle->active) {
      return;
    }
    const auto gen = static_cast<uint32_t>(msg.getNonNegInt("path_gen").value_or(0));
    if (bundle->retiring && bundle->retiring->gen == gen) {
      KeepRetiringAsStandby(*bundle);
    }
    (void)session->EnqueueOutbound(Utf8Body(BuildPathReleaseJson("path_release_ack", gen)));
  }

  /** Initiator: the release is acknowledged — close the old path. */
  void HandlePathReleaseAck(const pp::amp::ChannelSession* session, const Object& msg) {
    CallbackLock lock(*this);
    auto [bundle, path] = FindControlOwner(session);
    if (!bundle || path != &bundle->active || !bundle->retiring) {
      return;
    }
    const auto gen = static_cast<uint32_t>(msg.getNonNegInt("path_gen").value_or(0));
    if (bundle->retiring->gen == gen) {
      KeepRetiringAsStandby(*bundle);
    }
  }

  /**
   * k4 (K002): the released path stays as the call's warm standby — channels bound, slow heartbeat —
   * rather than closing. One standby; a relayed one is preferred (it survives NAT / network changes).
   */
  void KeepRetiringAsStandby(Bundle& bundle) {
    Path released = std::move(*bundle.retiring);
    bundle.retiring.reset();
    const bool keep = !PathLinkMissing(bundle, released) &&
                      (!bundle.standby || (released.kind == CallMediaLinkKind::Relayed &&
                                           bundle.standby->kind != CallMediaLinkKind::Relayed));
    if (!keep) {
      DropPath(bundle, released);
      CallMediaLegLog().info << "CallMediaLeg path released call_id=" << bundle.call_id << " path_gen=" << released.gen;
      return;
    }
    if (bundle.standby) {
      DropPath(bundle, *bundle.standby);
    }
    released.last_rx = Clock::now();
    CallMediaLegLog().info << "CallMediaLeg path released to standby call_id=" << bundle.call_id
                           << " path_gen=" << released.gen << " path="
                           << (released.kind == CallMediaLinkKind::Relayed ? "relayed" : "direct");
    bundle.standby = std::move(released);
  }

  /**
   * The active path died right after a switch while the path the call left is still retiring (bound,
   * alive, not yet released): TX goes back onto it — e.g. a punched link lost to the dual-dial election
   * a moment after the call moved onto it. The peer, losing the same link, does the same.
   */
  void FallBackToRetiring(Bundle& bundle, const std::string& why) {
    Path dead = std::move(bundle.active);
    bundle.active = std::move(*bundle.retiring);
    bundle.retiring.reset();
    bundle.active.last_rx = Clock::now();
    bundle.failed_over_at = Clock::now();
    bundle.release_sent = false;
    // The dead link is gone, so there is nothing to ping-pong with: another direct link to the peer
    // (the dual-dial winner) may be taken at once — an unused cold link lives only ~5 s.
    bundle.next_auto_migrate = {};
    DropPath(bundle, dead);
    CallMediaLegLog().warning << "CallMediaLeg back onto the path it left call_id=" << bundle.call_id
                              << " reason=" << why << " path_gen=" << bundle.active.gen << " path="
                              << (bundle.active.kind == CallMediaLinkKind::Relayed ? "relayed" : "direct");
    if (bundle.callbacks.on_path_changed) {
      pending_user_cbs.push_back([cb = bundle.callbacks.on_path_changed, kind = bundle.active.kind]() { cb(kind); });
    }
  }

  /** k4: TX onto the standby at once (no handshake — its channels are bound; RX takes every path). */
  void FailOverToStandby(Bundle& bundle, const std::string& why) {
    bundle.failed_over_at = Clock::now();
    Path old_active = std::move(bundle.active);
    bundle.active = std::move(*bundle.standby);
    bundle.standby.reset();
    bundle.active.last_rx = Clock::now();
    if (bundle.active.kind == CallMediaLinkKind::Relayed && old_active.kind == CallMediaLinkKind::Direct) {
      bundle.left_direct = old_active.link;
    }
    CallMediaLegLog().warning << "CallMediaLeg failover call_id=" << bundle.call_id << " reason=" << why << " to="
                              << (bundle.active.kind == CallMediaLinkKind::Relayed ? "relayed" : "direct")
                              << " path_gen=" << bundle.active.gen;
    if (!PathLinkMissing(bundle, old_active)) {
      old_active.last_rx = Clock::now();
      bundle.standby = std::move(old_active);  // silent, not gone: it may come back
    } else {
      DropPath(bundle, old_active);
    }
    if (bundle.callbacks.on_path_changed) {
      pending_user_cbs.push_back([cb = bundle.callbacks.on_path_changed, kind = bundle.active.kind]() { cb(kind); });
    }
  }

  /**
   * k4: the call lost its last path. It stays MediaReady on a dead active path for the reconnect
   * window; a migration onto a new link (the offerer re-anchors, the answerer accepts) recovers it.
   */
  void EnterPathLost(Bundle& bundle, const std::string& why) {
    if (bundle.Reconnecting()) {
      return;
    }
    const CallMediaLinkKind lost_kind = bundle.active.kind;
    const pp::amp::LinkHandle lost_link = bundle.active.link;
    Path lost;
    lost.gen = bundle.active.gen;  // the next path is gen + 1
    DropPath(bundle, bundle.active);
    bundle.active = std::move(lost);
    bundle.reconnect_deadline = Clock::now() + reconnect_window;
    CallMediaLegLog().warning << "CallMediaLeg path lost call_id=" << bundle.call_id << " reason=" << why
                              << " — reconnecting (window " << reconnect_window.count() << "ms)";
    pp::amp::PeerLink* other = QuietRebindTarget(bundle, lost_kind, lost_link);
    if (!other) {
      NotifyPathLost(bundle);
      return;
    }
    // The peer is still connected on another link (e.g. the loser of a dual-dial election was the
    // one the call had bound): the driver moves the call there now and nobody hears of the loss
    // unless that fails to land within the grace.
    bundle.path_lost_notify_at = Clock::now() + std::chrono::milliseconds(kCallMediaQuietRebindGraceMs);
    const bool drive = LocalWinsForBundle(bundle, *other);
    CallMediaLegLog().info << "CallMediaLeg quiet rebind call_id=" << bundle.call_id << " to="
                           << (other->IsCarrierBacked() ? "relayed" : "direct") << " drive=" << (drive ? 1 : 0)
                           << " phase=" << BundlePhaseName(bundle.phase);
    if (drive) {
      BeginMigrationLocked(bundle.leg_id, other->Handle(), [call_id = bundle.call_id](Roe<void> moved) {
        if (!moved) {
          CallMediaLegLog().info << "CallMediaLeg quiet rebind failed call_id=" << call_id << " ("
                                 << moved.error().message << ")";
        }
      });
    }
  }

  void NotifyPathLost(Bundle& bundle) {
    bundle.path_lost_notify_at = {};
    if (bundle.callbacks.on_path_lost) {
      pending_user_cbs.push_back(bundle.callbacks.on_path_lost);
    }
  }

  /** A Connected link to the peer other than `lost_link`: the lost path's class first. */
  pp::amp::PeerLink* QuietRebindTarget(const Bundle& bundle, const CallMediaLinkKind lost_kind,
                                       const pp::amp::LinkHandle lost_link) const {
    if (!runtime || bundle.remote_peer_id.empty()) {
      return nullptr;
    }
    const auto first = lost_kind == CallMediaLinkKind::Relayed ? pp::amp::TransportClass::Carrier
                                                               : pp::amp::TransportClass::Adp;
    const auto second =
        first == pp::amp::TransportClass::Adp ? pp::amp::TransportClass::Carrier : pp::amp::TransportClass::Adp;
    for (const auto transport : {first, second}) {
      pp::amp::PeerLink* link = runtime->Links().FindConnectedLinkByPeerId(bundle.remote_peer_id, transport);
      if (link && link->Mux() && link->Handle() != lost_link && link->Handle() != bundle.left_direct) {
        return link;
      }
    }
    return nullptr;
  }

  /** k4 heartbeat on a path's control channel (either direction), at most every `every`. */
  void MaybeHeartbeat(Path& path, const Clock::time_point now, const std::chrono::milliseconds every,
                      const bool active) {
    auto& control = path.outbound_control ? path.outbound_control : path.inbound_control;
    if (!control || now - path.last_hb_tx < every ||
        path.kind == silenced_kind_for_test.load(std::memory_order_relaxed)) {
      return;
    }
    path.last_hb_tx = now;
    (void)control->EnqueueOutbound(Utf8Body(HeartbeatJson(active)));
  }

  static int64_t MsSince(const Clock::time_point now, const Clock::time_point then) {
    return then.time_since_epoch().count() == 0
               ? 0
               : std::chrono::duration_cast<std::chrono::milliseconds>(now - then).count();
  }

  /** Initiator: migrate to the peer's Connected link of `kind` (direct ADP / relay carrier). */
  void BeginMigrationToKind(const CallMediaLegId leg_id, const CallMediaLinkKind kind, LegFinished done,
                            const bool standby_only = false) {
    CallbackLock lock(*this);
    Bundle* bundle = FindByLegId(leg_id);
    pp::amp::PeerLink* link =
        bundle && !bundle->remote_peer_id.empty()
            ? runtime->Links().FindConnectedLinkByPeerId(bundle->remote_peer_id,
                                                         kind == CallMediaLinkKind::Relayed
                                                             ? pp::amp::TransportClass::Carrier
                                                             : pp::amp::TransportClass::Adp)
            : nullptr;
    if (!link) {
      if (done) {
        pending_user_cbs.push_back([done = std::move(done), kind]() {
          done(Error(std::string("call-media migrate: no connected ") +
                     (kind == CallMediaLinkKind::Relayed ? "relayed" : "direct") + " link to the peer"));
        });
      }
      return;
    }
    BeginMigrationLocked(leg_id, link->Handle(), std::move(done), standby_only);
  }

  /**
   * k3: a relayed call whose peer is now reachable over a direct link (a punch landed, or it
   * dialed us) moves there — the driver starts it; the other side only answers. Never back onto
   * the direct link the call left for the relay (it had stopped delivering: TX-only escalation).
   */
  void MaybeAutoMigrate(Bundle& bundle, const Clock::time_point now) {
    if (!auto_migrate_to_direct.load(std::memory_order_relaxed) || bundle.active.kind != CallMediaLinkKind::Relayed ||
        bundle.migration || bundle.candidate || bundle.retiring || now < bundle.next_auto_migrate ||
        bundle.remote_peer_id.empty()) {
      return;
    }
    pp::amp::PeerLink* direct =
        runtime->Links().FindConnectedLinkByPeerId(bundle.remote_peer_id, pp::amp::TransportClass::Adp);
    if (!direct || direct->Mux() == bundle.active.mux || direct->Handle() == bundle.left_direct ||
        !LocalWinsForBundle(bundle, *direct)) {
      return;
    }
    bundle.next_auto_migrate = now + kAutoMigrateBackoff;
    CallMediaLegLog().info << "CallMediaLeg direct link up for relayed call_id=" << bundle.call_id
                           << " — migrating";
    BeginMigrationLocked(bundle.leg_id, direct->Handle(), {});
  }

  /** IO tick: candidate channel opens, migrate timeout, old-path release. Under `mu`. */
  void TickPaths(Bundle& bundle, const Clock::time_point now) {
    if (bundle.standby && PathLinkMissing(bundle, *bundle.standby)) {
      CallMediaLegLog().info << "CallMediaLeg standby link gone call_id=" << bundle.call_id;
      DropPath(bundle, *bundle.standby);
      bundle.standby.reset();
    }
    if (bundle.retiring && !bundle.candidate && PathLinkMissing(bundle, bundle.active) &&
        !PathLinkMissing(bundle, *bundle.retiring)) {
      FallBackToRetiring(bundle, "active link lost");
    }
    if (!bundle.candidate && !bundle.retiring) {
      CallMediaFailoverInput in;
      in.active_link_lost = PathLinkMissing(bundle, bundle.active);
      in.active_silence_ms = MsSince(now, bundle.active.last_rx);
      in.peer_heartbeats = bundle.peer_heartbeats;
      in.have_standby = bundle.standby.has_value();
      in.standby_link_alive = bundle.standby && !PathLinkMissing(bundle, *bundle.standby);
      in.standby_silence_ms = bundle.standby ? MsSince(now, bundle.standby->last_rx) : 0;
      if (bundle.failed_over_at.time_since_epoch().count() != 0) {
        in.since_failover_ms = MsSince(now, bundle.failed_over_at);
      }
      if (ShouldFailOverToStandby(in)) {
        FailOverToStandby(bundle, in.active_link_lost ? "active link lost" : "active path silent");
      } else if (in.active_link_lost && !bundle.Reconnecting()) {
        EnterPathLost(bundle, "active link lost, no standby");
      }
    }
    MaybeHeartbeat(bundle.active, now, std::chrono::milliseconds(kCallMediaActiveHeartbeatMs), true);
    if (bundle.standby) {
      MaybeHeartbeat(*bundle.standby, now, std::chrono::milliseconds(kCallMediaStandbyHeartbeatMs), false);
    }
    MaybeAutoMigrate(bundle, now);
    if (bundle.migration) {
      auto& m = *bundle.migration;
      pp::amp::PeerLink* link = bundle.candidate ? ResolvePathLink(bundle, *bundle.candidate) : nullptr;
      if (!link || !link->Mux()) {
        return AbandonMigrationLinkLost(bundle, "candidate link lost");
      }
      if (now >= m.deadline) {
        return AbandonMigration(bundle, "timed out");
      }
      if (m.initiator && m.pending_control != 0) {
        const auto state = link->Mux()->State(m.pending_control);
        if (state == pp::amp::ChannelState::Open) {
          const uint32_t channel = m.pending_control;
          m.pending_control = 0;
          BindControlChannel(bundle, *bundle.candidate, *link, channel, CallMediaChannelRole::OutboundControl);
          if (!bundle.candidate->outbound_control ||
              !bundle.candidate->outbound_control->EnqueueOutbound(Utf8Body(
                  BuildMigrateJson(bundle.call_id, bundle.params.media_epoch, bundle.candidate->gen,
                                   m.standby_only)))) {
            return AbandonMigration(bundle, "migrate send failed");
          }
        } else if (state == pp::amp::ChannelState::Closed) {
          return AbandonMigration(bundle, "candidate control channel refused");
        }
      }
      if (m.initiator && m.pending_media != 0) {
        const auto state = link->Mux()->State(m.pending_media);
        if (state == pp::amp::ChannelState::Open) {
          const uint32_t channel = m.pending_media;
          m.pending_media = 0;
          BindMediaChannel(bundle, *bundle.candidate, *link, channel);
          CompleteCandidate(bundle);
        } else if (state == pp::amp::ChannelState::Closed) {
          return AbandonMigration(bundle, "candidate media channel refused");
        }
      }
    }
    if (bundle.retiring) {
      if (PathLinkMissing(bundle, *bundle.retiring) || now - bundle.switched_at >= kRetireAbandon) {
        DropPath(bundle, *bundle.retiring);
        bundle.retiring.reset();
        return;
      }
      if (bundle.drove_switch && !bundle.release_sent && bundle.active.outbound_control &&
          now - bundle.switched_at >= kRetireAfterSwitch &&
          (bundle.rx_since_switch || now - bundle.switched_at >= kRetireAtMost)) {
        bundle.release_sent = bundle.active.outbound_control->EnqueueOutbound(
            Utf8Body(BuildPathReleaseJson("path_release", bundle.retiring->gen)));
        bundle.release_sent_at = now;
      }
    }
  }

  void HandleInboundHello(const std::shared_ptr<pp::amp::ChannelSession>& channel_session, const Object& hello) {
    if (!channel_session) {
      return;
    }
    const std::string hello_call_id = hello.getString("call_id").value_or("");
    if (hello_call_id.empty()) {
      return;
    }

    pp::amp::PeerLink* link = nullptr;
    {
      CallbackLock lock(*this);
      Bundle* holder = FindByInboundSession(channel_session);
      if (!holder) {
        return;
      }
      link = ResolveLink(*holder);
      if (!link) {
        return;
      }

      Bundle* target = FindByCallId(hello_call_id);
      if (IsPendingCallId(holder->call_id) && target && target != holder && target->Reconnecting()) {
        // k4: a peer that lost the path the old way (tear down, redial) sends a fresh hello: take it
        // instead of waiting out the window for a migration it will never send.
        CallMediaLegLog().info << "CallMediaLeg fresh hello replaces reconnecting call_id=" << hello_call_id;
        EraseBundle(hello_call_id);
        target = nullptr;
      }
      if (IsPendingCallId(holder->call_id) && target && target != holder) {
        // A hello on a new channel for a call that already has a bundle: decide against that
        // bundle BEFORE touching it. A rejected hello (e.g. a second hello for a live call) must
        // answer and close only its own channel — evicting the live inbound control first sent
        // the peer a Close and failed the call.
        const auto saved_remote_offerer = target->remote_offerer;
        target->remote_offerer = hello.getString("role").value_or("") == "offerer";
        CallMediaInboundHelloContext pre;
        pre.phase = target->phase;
        pre.has_outbound_control = static_cast<bool>(target->active.outbound_control);
        pre.offerer = target->offerer;
        pre.local_wins_glare = LocalWinsForBundle(*target, *link);
        pre.other_bundle_busy = OtherBundleBusy(hello_call_id);
        const auto pre_decision = DecideCallMediaInboundHello(pre);
        if (pre_decision == CallMediaInboundHelloDecision::RejectBusy ||
            pre_decision == CallMediaInboundHelloDecision::RejectGlare) {
          target->remote_offerer = saved_remote_offerer;
          const char* reason = pre_decision == CallMediaInboundHelloDecision::RejectBusy ? "busy" : "glare";
          CallMediaLegLog().info << "CallMediaLeg inbound hello reject call_id=" << hello_call_id
                                 << " peer=" << link->PeerKey() << " reason=" << reason
                                 << " (existing bundle kept)";
          (void)channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(false, reason)));
          EraseBundle(holder->call_id);  // the placeholder and its new channel only
          return;
        }
      }
      if (IsPendingCallId(holder->call_id)) {
        if (target && target != holder) {
          DropRole(*target, CallMediaChannelRole::InboundControl);
          target->active.inbound_control = std::move(holder->active.inbound_control);
          if (target->params.peer_key.empty()) {
            target->params.peer_key = link->PeerKey();
          }
          if (target->active.inbound_control) {
            IndexChannel(target->active.inbound_control->ChannelId(), hello_call_id, CallMediaChannelRole::InboundControl);
          }
          EraseBundle(holder->call_id);
          holder = target;
        } else {
          RekeyPendingToCallId(*holder, hello_call_id);
          target = FindByCallId(hello_call_id);
          if (target) {
            // A call born from the peer's hello: we hold the other role. (An answerer's hello can
            // reach the offerer before its own media starts; the offerer then joins this bundle.)
            target->offerer = hello.getString("role").value_or("") != "offerer";
          }
        }
      } else {
        target = holder;
      }
      if (!target) {
        return;
      }

      // Record the dialer's role before deciding glare (antisymmetric winner needs both roles).
      target->remote_offerer = hello.getString("role").value_or("") == "offerer";
      CallMediaInboundHelloContext ctx;
      ctx.phase = target->phase;
      ctx.has_outbound_control = static_cast<bool>(target->active.outbound_control);
      ctx.offerer = target->offerer;
      ctx.local_wins_glare = LocalWinsForBundle(*target, *link);
      ctx.other_bundle_busy = OtherBundleBusy(hello_call_id);
      const auto decision = DecideCallMediaInboundHello(ctx);

      if (decision == CallMediaInboundHelloDecision::RejectBusy) {
        CallMediaLegLog().info << "CallMediaLeg inbound hello reject call_id="
                               << hello_call_id << " peer=" << link->PeerKey() << " reason=busy";
        (void)channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(false, "busy")));
        DropRole(*target, CallMediaChannelRole::InboundControl);
        if (!target->active.outbound_control && target->phase == CallMediaBundlePhase::Idle) {
          EraseBundle(target->call_id);
        }
        return;
      }
      if (decision == CallMediaInboundHelloDecision::RejectGlare) {
        CallMediaLegLog().info << "CallMediaLeg inbound hello reject call_id="
                               << hello_call_id << " peer=" << link->PeerKey() << " reason=glare";
        (void)channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(false, "glare")));
        DropRole(*target, CallMediaChannelRole::InboundControl);
        return;
      }
      if (decision == CallMediaInboundHelloDecision::AcceptAndYield) {
        DropRole(*target, CallMediaChannelRole::OutboundControl);
        target->control_ready = false;
      }
      target->phase = CallMediaBundlePhase::InboundHello;
      if (target->params.peer_key.empty()) {
        target->params.peer_key = link->PeerKey();
      }
      target->finished = false;
      if (target->deadline.time_since_epoch().count() == 0) {
        target->deadline = Clock::now() + std::chrono::seconds(15);
      }
      CallMediaLegLog().info << "CallMediaLeg inbound hello call_id=" << hello_call_id
                             << " peer=" << link->PeerKey()
                             << " decision="
                             << (decision == CallMediaInboundHelloDecision::AcceptAndYield ? "yield"
                                 : decision == CallMediaInboundHelloDecision::Accept        ? "accept"
                                                                                            : "other")
                             << " local_offerer=" << (target->offerer ? 1 : 0);
    }

    CallMediaDirectConnectParams params;
    params.call_id = hello_call_id;
    params.media_epoch = static_cast<uint32_t>(hello.getNonNegInt("media_epoch").value_or(1));
    params.offerer = hello.getString("role").value_or("") == "offerer";
    params.peer_key = link->PeerKey();

    InboundHandler handler;
    {
      CallbackLock lock(*this);
      handler = inbound;
    }
    if (!handler) {
      (void)channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(false, "no handler")));
      CallbackLock lock(*this);
      auto* bundle = FindByCallId(hello_call_id);
      if (bundle) {
        DropRole(*bundle, CallMediaChannelRole::InboundControl);
        if (!bundle->active.outbound_control) {
          EraseBundle(hello_call_id);
        } else {
          bundle->phase = CallMediaBundlePhase::OutboundHello;
        }
      }
      return;
    }

    const std::string peer_key = link->PeerKey();
    // The handler answers asynchronously (the calls owner waits for the media key without a
    // thread). Asked after the mux stack unwinds; the answer comes back onto IO.
    auto answer = [this, self = shared_from_this(), channel_session, peer_key,
                   call_id = hello_call_id](CallMediaDirectConnectParams answer_params,
                                            CallMediaDirectCallbacks answer_cbs) mutable {
      PostIo([this, self, channel_session, answer_params = std::move(answer_params),
              answer_cbs = std::move(answer_cbs), peer_key, call_id]() mutable {
        CallbackLock lock(*this);
        auto* bundle = FindByCallId(call_id);
        if (!bundle || bundle->phase != CallMediaBundlePhase::InboundHello) {
          return;
        }
        pp::amp::PeerLink* resolved = ResolveLink(*bundle);
        if (!resolved) {
          return;
        }
        if (LocalWinsForBundle(*bundle, *resolved) && bundle->active.outbound_control) {
          DropRole(*bundle, CallMediaChannelRole::InboundControl);
          bundle->phase = CallMediaBundlePhase::OutboundHello;
          return;
        }
        if (answer_params.media_key.empty() || answer_params.call_id.empty()) {
          (void)channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(false, "rejected")));
          TearDownBundle(*bundle, false, false, "amp call-media: inbound rejected");
          return;
        }
        if (!bundle->leg_id) {
          bundle->leg_id = CallMediaLegId{next_leg_id.fetch_add(1, std::memory_order_relaxed)};
        }
        bundle->params = answer_params;
        bundle->callbacks = std::move(answer_cbs);
        // Bridge inbound handler remaps peer_key to account: for mixer stream ids. Amp PeerLink
        // stays under mesh PeerId — keep that for ResolveLink / PeerLinkMissing (HL004 dual-SNAT).
        if (!peer_key.empty()) {
          bundle->params.peer_key = peer_key;
        }
        bundle->phase = CallMediaBundlePhase::AwaitingMedia;
        if (!channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(true)))) {
          TearDownBundle(*bundle, false, false, "amp call-media: hello ack failed");
          return;
        }
        bundle->control_ready = true;
        TryEnterMediaReady(*bundle);
      });
    };
    PostIo([self = shared_from_this(), params, handler = std::move(handler), answer = std::move(answer)]() mutable {
      handler(params, std::move(answer));
    });
  }

  void HandleHelloAck(const std::string& call_id, const CallMediaChannelRole role, const Object& ack) {
    CallbackLock lock(*this);
    auto* bundle = FindByCallId(call_id);
    if (!bundle) {
      return;
    }
    CallMediaHelloAckContext ctx;
    ctx.phase = bundle->phase;
    ctx.ack_ok = ack.getIf<bool>("ok").value_or(false);
    ctx.from_outbound_control = role == CallMediaChannelRole::OutboundControl;
    ctx.offerer = bundle->offerer;
    ctx.local_wins_glare = [&] {
      if (auto* resolved = ResolveLink(*bundle)) {
        return LocalWinsForBundle(*bundle, *resolved);
      }
      return true;
    }();
    switch (DecideCallMediaHelloAck(ctx)) {
    case CallMediaHelloAckDecision::IgnoreStale:
      return;
    case CallMediaHelloAckDecision::YieldOutbound:
      DropRole(*bundle, CallMediaChannelRole::OutboundControl);
      bundle->control_ready = false;
      if (bundle->active.inbound_control) {
        bundle->phase = CallMediaBundlePhase::InboundHello;
      }
      return;
    case CallMediaHelloAckDecision::Fail:
      TearDownBundle(*bundle, false, false, "amp call-media: hello rejected");
      return;
    case CallMediaHelloAckDecision::ProceedToMedia:
      bundle->control_ready = true;
      bundle->phase = CallMediaBundlePhase::AwaitingMedia;
      OpenMediaOutbound(*bundle);
      return;
    }
  }

  void HandleInboundChannel(pp::amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !link.Mux()) {
      return;
    }
    const auto cls = link.Mux()->Class(channel_id);
    if (cls == pp::amp::ChannelClass::RealtimeControl) {
      CallbackLock lock(*this);
      auto pending = std::make_unique<Bundle>();
      pending->leg_id = CallMediaLegId{next_leg_id.fetch_add(1, std::memory_order_relaxed)};
      // Unique per placeholder: channel ids restart at 1 on every link, so a placeholder left on
      // one link (an ignored migrate) collided with the next link's channel 1 — the emplace below
      // kept the old entry and `raw` dangled (use-after-free).
      pending->call_id = std::string("__pending_") + std::to_string(pending->leg_id.value) + "_" + std::to_string(channel_id);
      pending->params.peer_key = link.PeerKey();
      pending->deadline = Clock::now() + std::chrono::seconds(15);
      pending->finished = true;
      pending->phase = CallMediaBundlePhase::Idle;
      const std::string pending_id = pending->call_id;
      auto* raw = pending.get();
      if (!bundles.emplace(pending_id, std::move(pending)).second) {
        return;
      }
      BindControlChannel(*raw, link, channel_id, CallMediaChannelRole::InboundControl);
      return;
    }
    if (cls == pp::amp::ChannelClass::Realtime) {
      CallbackLock lock(*this);
      // k3: the driver's media channel on our accepted candidate path completes the switch.
      for (auto& [_, bundle] : bundles) {
        if (bundle && bundle->candidate && bundle->migration && !bundle->migration->initiator &&
            !bundle->candidate->media_bound && bundle->candidate->mux == link.Mux()) {
          BindMediaChannel(*bundle, *bundle->candidate, link, channel_id);
          CompleteCandidate(*bundle);
          return;
        }
      }
      Bundle* target = nullptr;
      for (auto& [_, bundle] : bundles) {
        if (bundle && BundleMatchesLink(*bundle, link) && bundle->phase == CallMediaBundlePhase::AwaitingMedia &&
            !bundle->active.media_bound) {
          target = bundle.get();
          break;
        }
      }
      if (!target) {
        for (auto& [_, bundle] : bundles) {
          if (bundle && BundleMatchesLink(*bundle, link) && bundle->control_ready && !bundle->active.media_bound) {
            target = bundle.get();
            break;
          }
        }
      }
      if (!target) {
        return;
      }
      BindMediaChannel(*target, link, channel_id);
      TryEnterMediaReady(*target);
    }
  }

  /** Dual-dial: StartLeg arrived after inbound already claimed this call_id. */
  bool AdoptOutboundIntoExisting(Bundle& existing, const CallMediaLegId leg_id, const bool local_offerer,
                                 CallMediaDirectCallbacks& callbacks, LegFinished& on_finished,
                                 const int timeout_ms) {
    const auto phase = existing.phase;
    if (phase != CallMediaBundlePhase::InboundHello && phase != CallMediaBundlePhase::AwaitingMedia &&
        phase != CallMediaBundlePhase::MediaReady) {
      return false;
    }
    existing.leg_id = leg_id;
    existing.offerer = local_offerer;  // our own role: authoritative over what the inbound hello implied
    existing.deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 15000);
    // Inbound worker may not have installed answer callbacks yet.
    if (!existing.callbacks.on_audio && !existing.callbacks.on_media && !existing.callbacks.on_connected) {
      existing.callbacks = std::move(callbacks);
    }
    if (existing.finished) {
      // MediaReady (or failed) settled before dialer callback was attached.
      LegFinished cb = std::move(on_finished);
      if (cb) {
        Roe<void> result = phase == CallMediaBundlePhase::MediaReady ? Roe<void>{} : Error("call-media aborted");
        pending_user_cbs.push_back([cb = std::move(cb), result = std::move(result)]() mutable { cb(std::move(result)); });
      }
      return true;
    }
    existing.on_finished = std::move(on_finished);
    return true;
  }

  /** One outbound leg's control-channel open (retried while the association is not ready). */
  struct OutboundOpen {
    CallMediaLegId leg_id;
    std::string peer_key;
    std::string call_id;
    CallMediaDirectConnectParams params;
    Clock::time_point deadline;
  };

  /**
   * Open the leg's control channel on a Connected link to the peer (#235 — the relay carrier, even
   * while a redial holds the dial key), dialing first when none is up. Each pass resolves again, so
   * a carrier that comes up mid-dial is used; the channel is always opened on a known link handle.
   */
  void OpenOutboundControl(OutboundOpen open, const int retries) {
    if (retries == 0) {
      CallMediaLegLog().info << "CallMediaLeg OpenChannel invoke call_id=" << open.call_id
                             << " peer=" << open.peer_key;
    }
    auto on_channel = [this, self = shared_from_this(), open, retries](const pp::amp::LinkHandle bound,
                                                                         pp::amp::PeerLinkManager::ChannelRoe channel) {
      OnOutboundControlOpened(open, retries, bound, std::move(channel));
    };
    // Any Connected link to the peer (the relay carrier included) — a dial in flight under the key
    // must not hide it (#235). The handle pins the wait to that link.
    if (pp::amp::PeerLink* link = runtime->Links().ResolveConnectedLink(open.peer_key)) {
      const pp::amp::LinkHandle bound = link->Handle();
      if (retries > 0 || link->IsCarrierBacked()) {
        CallMediaLegLog().info << "CallMediaLeg OpenChannel on connected link call_id=" << open.call_id
                               << " peer=" << open.peer_key
                               << " path=" << (link->IsCarrierBacked() ? "relayed" : "direct");
      }
      runtime->Links().OpenChannelOnLink(
          *link, kCallMediaDirectProtocolId, pp::amp::CallMediaControlChannelPolicy(),
          [on_channel, bound](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
            on_channel(bound, std::move(channel));
          });
      return;
    }
    // Nothing up: dial, then open on the link the next pass resolves. Never open by key — the
    // channel id belongs to one mux, and its link must be the one we wait on and bind (PR #239).
    // The association being up does not mean a Connected link resolves under the key (it may not yet,
    // or the key maps elsewhere): report it as not ready, so the next pass goes through the retry
    // limit, the deadline and the bundle's liveness like any other wait — never a bare re-post.
    runtime->Links().EnsureAssociation(
        open.peer_key, [on_channel](pp::amp::PeerLinkManager::LinkRoe associated) mutable {
          using Links = pp::amp::PeerLinkManager;
          on_channel(pp::amp::LinkHandle{},
                     Links::ChannelRoe::error(associated ? Links::Failure::Of(Links::Err::AssociationNotReady,
                                                                              "amp call-media: no connected link yet")
                                                         : associated.error()));
        });
  }

  void OnOutboundControlOpened(const OutboundOpen& open, const int retries, const pp::amp::LinkHandle bound,
                               pp::amp::PeerLinkManager::ChannelRoe channel) {
    CallbackLock lock(*this);
    auto* bundle = FindByCallId(open.call_id);
    if (!bundle || bundle->leg_id.value != open.leg_id.value) {
      return;
    }
    if (!channel) {
      const bool assoc_not_ready = pp::amp::PeerLinkManager::IsAssociationNotReady(channel.error());
      if (assoc_not_ready && retries < 500 && !bundle->finished && Clock::now() < open.deadline) {
        if (retries == 0 || (retries % 50) == 0) {
          CallMediaLegLog().info << "CallMediaLeg OpenChannel wait call_id=" << open.call_id
                                 << " peer=" << open.peer_key << " retries=" << retries
                                 << " err=" << channel.error().message;
        }
        lock.unlock();
        PostIo([this, self = shared_from_this(), open, retries]() { OpenOutboundControl(open, retries + 1); });
        return;
      }
      CallMediaLegLog().info << "CallMediaLeg OpenChannel fail call_id=" << open.call_id << " peer=" << open.peer_key
                             << " retries=" << retries << " err=" << channel.error().message;
      if (bundle->phase == CallMediaBundlePhase::OutboundHello) {
        TearDownBundle(*bundle, false, false, channel.error().message);
      }
      return;
    }
    if (retries > 0) {
      CallMediaLegLog().info << "CallMediaLeg OpenChannel ok call_id=" << open.call_id << " peer=" << open.peer_key
                             << " after_retries=" << retries;
    }
    auto* link = LiveLink(bound);  // the link the channel was opened on (its id is that mux's)
    if (!link) {
      CallMediaLegLog().info << "CallMediaLeg OpenChannel ok but link missing call_id=" << open.call_id
                             << " peer=" << open.peer_key;
      if (bundle->phase == CallMediaBundlePhase::OutboundHello) {
        TearDownBundle(*bundle, false, false, "amp call-media: peer link missing");
      }
      return;
    }
    // Glare loser (or inbound-first admit) already left OutboundHello — abandon this open.
    if (bundle->phase != CallMediaBundlePhase::OutboundHello) {
      if (link->Mux()) {
        (void)link->Mux()->CloseChannel(*channel, "call-media glare yield");
      }
      return;
    }
    const pp::amp::LinkHandle handle = link->Handle();
    const uint32_t channel_id = *channel;
    lock.unlock();
    ScheduleWhenChannelOpen(handle, channel_id, open.deadline,
                            [this, self = shared_from_this(), open, handle, channel_id](const bool opened) {
                              OnOutboundControlReady(open, handle, channel_id, opened);
                            });
  }

  void OnOutboundControlReady(const OutboundOpen& open, const pp::amp::LinkHandle handle, const uint32_t channel_id,
                              const bool opened) {
    CallbackLock lock(*this);
    auto* bundle = FindByCallId(open.call_id);
    if (!bundle || bundle->leg_id.value != open.leg_id.value) {
      return;
    }
    auto* link = LiveLink(handle);
    if (!opened) {
      if (bundle->phase == CallMediaBundlePhase::OutboundHello) {
        CallMediaLegLog().info << "CallMediaLeg channel open failed call_id=" << open.call_id
                               << " peer=" << open.peer_key;
        TearDownBundle(*bundle, false, false, "amp call-media: channel open failed");
      } else if (link && link->Mux()) {
        (void)link->Mux()->CloseChannel(channel_id, "call-media glare yield");
      }
      return;
    }
    if (bundle->phase != CallMediaBundlePhase::OutboundHello) {
      if (link && link->Mux()) {
        (void)link->Mux()->CloseChannel(channel_id, "call-media glare yield");
      }
      return;
    }
    if (!link) {
      TearDownBundle(*bundle, false, false, "amp call-media: peer link missing");
      return;
    }
    BindControlChannel(*bundle, *link, channel_id, CallMediaChannelRole::OutboundControl);
    if (!bundle->active.outbound_control ||
        !bundle->active.outbound_control->EnqueueOutbound(Utf8Body(BuildHelloJson(open.params)))) {
      CallMediaLegLog().info << "CallMediaLeg hello write failed call_id=" << open.call_id
                             << " peer=" << open.peer_key;
      TearDownBundle(*bundle, false, false, "amp call-media: hello write failed");
      return;
    }
    CallMediaLegLog().info << "CallMediaLeg hello sent call_id=" << open.call_id << " peer=" << open.peer_key
                           << " role=" << (open.params.offerer ? "offerer" : "answerer");
  }

  void BeginOutboundLeg(const CallMediaLegId leg_id, const CallMediaDirectConnectParams& params,
                        CallMediaDirectCallbacks callbacks, LegFinished on_finished, const int timeout_ms) {
    if (stopped.load(std::memory_order_acquire)) {
      if (on_finished) {
        on_finished(Error("call-media aborted"));
      }
      return;
    }
    const std::string peer_key = params.peer_key;
    const std::string call_id = params.call_id;
    Clock::time_point deadline;
    {
      CallbackLock lock(*this);
      if (auto* existing = FindByCallId(params.call_id)) {
        if (AdoptOutboundIntoExisting(*existing, leg_id, params.offerer, callbacks, on_finished, timeout_ms)) {
        return;
      }
        TearDownBundle(*existing, true, false, "call-media aborted");
      }
      std::vector<std::string> others;
      for (auto& [id, bundle] : bundles) {
        if (bundle && CallMediaBundlePhaseIsActive(bundle->phase) && !IsPendingCallId(id)) {
          others.push_back(id);
        }
      }
      for (const auto& id : others) {
        if (auto* b = FindByCallId(id)) {
          TearDownBundle(*b, true, false, "call-media aborted");
        }
      }
      auto bundle = std::make_unique<Bundle>();
      bundle->leg_id = leg_id;
      bundle->call_id = params.call_id;
      bundle->params = params;
      bundle->callbacks = std::move(callbacks);
      bundle->on_finished = std::move(on_finished);
      bundle->finished = false;
      bundle->offerer = params.offerer;
      bundle->deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 15000);
      bundle->phase = CallMediaBundlePhase::OutboundHello;
      deadline = bundle->deadline;
      bundles.emplace(params.call_id, std::move(bundle));
    }

    {
      const auto snap = runtime->Links().GetLinkSnapshot(peer_key);
      const bool connected = runtime->Links().IsConnected(peer_key);
      std::string ma;
      if (auto preferred = runtime->Links().PreferredMultiaddr(peer_key)) {
        ma = *preferred;
      }
      CallMediaLegLog().info << "CallMediaLeg outbound begin call_id=" << call_id
                             << " peer=" << peer_key
                             << " role=" << (params.offerer ? "offerer" : "answerer")
                             << " has_endpoint=" << (snap.has_endpoint ? 1 : 0)
                             << " connected=" << (connected ? 1 : 0)
                             << " ma=" << (ma.empty() ? "(none)" : ma);
    }

    OpenOutboundControl(OutboundOpen{leg_id, peer_key, call_id, params, deadline}, 0);
  }
};

CallMediaLegCoordinator::CallMediaLegCoordinator(pp::amp::MeshRuntime& runtime)
    : impl_(std::make_shared<Impl>()), runtime_(runtime) {
  impl_->runtime = &runtime_;
}

CallMediaLegCoordinator::~CallMediaLegCoordinator() {
  Stop();
}

void CallMediaLegCoordinator::Start() {
  if (impl_->started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  impl_->runtime = &runtime_;
  impl_->stopped.store(false, std::memory_order_release);
  impl_->io_tick_id = runtime_.AddIoTick([weak = std::weak_ptr(impl_)] {
    if (auto impl = weak.lock()) {
      impl->TickDeadlines();
    }
  });
  runtime_.Links().SetProtocolHandler(
      kCallMediaDirectProtocolId,
      [weak = std::weak_ptr(impl_)](pp::amp::LinkHandle handle, const std::string& /*remote_peer_id*/,
                                   const uint32_t ch) {
        if (auto impl = weak.lock()) {
          if (!impl->runtime) {
            return;
          }
          impl->runtime->Links().WithLiveLink(handle, [&](pp::amp::PeerLink& link) {
            impl->HandleInboundChannel(link, ch);
          });
        }
      });
}

void CallMediaLegCoordinator::Stop() {
  // Idempotent: the destructor Stops again, possibly after MeshHost::Stop freed the runtime.
  if (!impl_->started.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.RemoveIoTick(impl_->io_tick_id);
  impl_->io_tick_id = 0;
  runtime_.Links().RemoveProtocolHandler(kCallMediaDirectProtocolId);
  // Tear down synchronously: a PostIo(raw Impl*) races if the caller destroys then Pumps
  // (macOS: "mutex lock failed: Invalid argument"). Strand before the coordinator lock — IO
  // callbacks hold the strand, so an off-IO Stop taking `mu` first can invert against MeshPump
  // (see CircuitClientCoordinator::AbortInflight).
  runtime_.WithIoLock([this]() {
    Impl::CallbackLock lock(*impl_);
    std::vector<std::string> ids;
    for (auto& [id, _] : impl_->bundles) {
      ids.push_back(id);
    }
    for (const auto& id : ids) {
      if (auto* b = impl_->FindByCallId(id)) {
        impl_->TearDownBundle(*b, true, false, "call-media aborted");
      }
    }
  });
  ClearInboundHandler();
  // Poison already-queued PostIo(self) work before dropping runtime.
  impl_->io_deferred.Invalidate();
  // Drop runtime before callers destroy MeshRuntime / harness (posted answers may still land).
  impl_->runtime = nullptr;
}

void CallMediaLegCoordinator::SetInboundHandler(InboundHandler handler) {
  Impl::CallbackLock lock(*impl_);
  impl_->inbound = std::move(handler);
}

void CallMediaLegCoordinator::ClearInboundHandler() {
  Impl::CallbackLock lock(*impl_);
  impl_->inbound = nullptr;
}

CallMediaLegId CallMediaLegCoordinator::StartLeg(const CallMediaDirectConnectParams& params,
                                                 CallMediaDirectCallbacks callbacks, LegFinished on_finished,
                                                 const int timeout_ms) {
  if (!impl_->started.load(std::memory_order_acquire)) {
    if (on_finished) {
      runtime_.PostToIo(
          [on_finished = std::move(on_finished)]() mutable { on_finished(Error("call-media service not started")); });
    }
    return {};
  }
  if (params.peer_key.empty() || params.call_id.empty() || params.media_key.empty()) {
    if (on_finished) {
      const char* which = params.peer_key.empty()   ? "peer_key"
                          : params.call_id.empty()  ? "call_id"
                                                    : "media_key";
      CallMediaLegLog().info << "CallMediaLeg reject invalid params missing=" << which
                             << " call_id=" << params.call_id << " peer=" << params.peer_key
                             << " media_key_len=" << params.media_key.size();
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Error("amp call-media: invalid connect params"));
      });
    }
    return {};
  }
  // Direct ADP endpoint or circuit-backed nested Session ([A024]) already Connected.
  if (!runtime_.Links().GetLinkSnapshot(params.peer_key).has_endpoint &&
      !runtime_.Links().IsConnected(params.peer_key)) {
    if (on_finished) {
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Error("amp call-media: peer not reachable (no endpoint or nested link)"));
      });
    }
    return {};
  }

  const CallMediaLegId leg_id{impl_->next_leg_id.fetch_add(1, std::memory_order_relaxed)};
  impl_->PostIo([impl = impl_, leg_id, params, callbacks = std::move(callbacks),
                 on_finished = std::move(on_finished), timeout_ms]() mutable {
    impl->BeginOutboundLeg(leg_id, params, std::move(callbacks), std::move(on_finished), timeout_ms);
  });
  return leg_id;
}

void CallMediaLegCoordinator::CancelLeg(const CallMediaLegId id) {
  impl_->PostIo([impl = impl_, id]() {
    Impl::CallbackLock lock(*impl);
    if (auto* b = impl->FindByLegId(id)) {
      impl->TearDownBundle(*b, true, false, "call-media aborted");
    }
  });
}

void CallMediaLegCoordinator::DetachLeg(const CallMediaLegId id) {
  impl_->PostIo([impl = impl_, id]() {
    Impl::CallbackLock lock(*impl);
    if (auto* b = id ? impl->FindByLegId(id) : impl->PrimaryBundle()) {
      impl->TearDownBundle(*b, true, false, "call-media aborted");
    }
  });
  runtime_.Pump();
}

bool CallMediaLegCoordinator::IsLegActive(const CallMediaLegId id) const {
  if (!id) {
    return false;
  }
  Impl::CallbackLock lock(*impl_);
  const auto* bundle = impl_->FindByLegId(id);
  return bundle && CallMediaBundlePhaseIsActive(bundle->phase);
}

bool CallMediaLegCoordinator::IsActive() const {
  Impl::CallbackLock lock(*impl_);
  const auto* bundle = impl_->PrimaryBundle();
  return bundle && CallMediaBundlePhaseIsActive(bundle->phase);
}

CallMediaLegId CallMediaLegCoordinator::PrimaryLegId() const {
  Impl::CallbackLock lock(*impl_);
  const auto* bundle = impl_->PrimaryBundle();
  return bundle ? bundle->leg_id : CallMediaLegId{};
}

CallMediaDirectConnectParams CallMediaLegCoordinator::ActiveParams() const {
  Impl::CallbackLock lock(*impl_);
  const auto* bundle = impl_->PrimaryBundle();
  return bundle ? bundle->params : CallMediaDirectConnectParams{};
}

std::string CallMediaLegCoordinator::ActiveRemotePeerId() const {
  Impl::CallbackLock lock(*impl_);
  const auto* bundle = impl_->PrimaryBundle();
  return bundle ? bundle->remote_peer_id : std::string{};
}

CallLinkCounters CallMediaLegCoordinator::ActiveLinkCounters() const {
  pp::amp::LinkHandle link{};
  {
    Impl::CallbackLock lock(*impl_);
    const auto* bundle = impl_->PrimaryBundle();
    if (!bundle || bundle->active.kind == CallMediaLinkKind::Unknown) {
      return {};
    }
    link = bundle->active.link;
  }
  // The IO strand takes this coordinator's lock inside link callbacks: never nest the other way.
  const auto stats = runtime_.Links().LinkConnectionStats(link);
  if (!stats) {
    return {};
  }
  return CallLinkCounters{.available = true,
                          .link_id = link.id.value,
                          .reliable_sent = stats->reliable_sent,
                          .retransmits = stats->retransmits,
                          .srtt_ms = stats->srtt_ms};
}

CallMediaLinkKind CallMediaLegCoordinator::ActiveLinkKind() const {
  Impl::CallbackLock lock(*impl_);
  const auto* bundle = impl_->PrimaryBundle();
  return bundle ? bundle->active.kind : CallMediaLinkKind::Unknown;
}

CallMediaLegPhase CallMediaLegCoordinator::LegPhase(const CallMediaLegId id) const {
  if (!id) {
    return CallMediaLegPhase::Closed;
  }
  Impl::CallbackLock lock(*impl_);
  const auto* bundle = impl_->FindByLegId(id);
  if (!bundle) {
    return CallMediaLegPhase::Closed;
  }
  return CallMediaBundlePhaseToLegPhase(bundle->phase);
}

CallMediaSessionPhase CallMediaLegCoordinator::Phase() const {
  Impl::CallbackLock lock(*impl_);
  const auto* bundle = impl_->PrimaryBundle();
  if (!bundle) {
    return CallMediaSessionPhase::Idle;
  }
  return CallMediaBundlePhaseToSessionPhase(bundle->phase);
}

CallMediaBundlePhase CallMediaLegCoordinator::BundlePhase(const CallMediaLegId id) const {
  if (!id) {
    return CallMediaBundlePhase::Idle;
  }
  Impl::CallbackLock lock(*impl_);
  const auto* bundle = impl_->FindByLegId(id);
  return bundle ? bundle->phase : CallMediaBundlePhase::Idle;
}

void CallMediaLegCoordinator::MigrateLeg(const CallMediaLegId id, const pp::amp::LinkHandle link, LegFinished done) {
  impl_->PostIo([impl = impl_, id, link, done = std::move(done)]() mutable {
    impl->BeginMigration(id, link, std::move(done));
  });
  runtime_.Pump();
}

void CallMediaLegCoordinator::SetIgnoreMigrateForTest(const bool ignore) {
  impl_->ignore_migrate_for_test.store(ignore, std::memory_order_relaxed);
}

void CallMediaLegCoordinator::MigrateLegToKind(const CallMediaLegId id, const CallMediaLinkKind kind,
                                               LegFinished done) {
  impl_->PostIo([impl = impl_, id, kind, done = std::move(done)]() mutable {
    impl->BeginMigrationToKind(id, kind, std::move(done));
  });
  runtime_.Pump();
}

void CallMediaLegCoordinator::AddStandbyLegOfKind(const CallMediaLegId id, const CallMediaLinkKind kind,
                                                  LegFinished done) {
  impl_->PostIo([impl = impl_, id, kind, done = std::move(done)]() mutable {
    impl->BeginMigrationToKind(id, kind, std::move(done), /*standby_only=*/true);
  });
  runtime_.Pump();
}

void CallMediaLegCoordinator::SetAutoMigrateToDirect(const bool enable) {
  impl_->auto_migrate_to_direct.store(enable, std::memory_order_relaxed);
}

void CallMediaLegCoordinator::SetSilencedPathKindForTest(const CallMediaLinkKind kind) {
  impl_->silenced_kind_for_test.store(kind, std::memory_order_relaxed);
}

void CallMediaLegCoordinator::SetReconnectWindowForTest(const std::chrono::milliseconds window) {
  Impl::CallbackLock lock(*impl_);
  impl_->reconnect_window = window;
}

void CallMediaLegCoordinator::SetMigrateTimeoutForTest(const std::chrono::milliseconds timeout) {
  Impl::CallbackLock lock(*impl_);
  impl_->migrate_timeout = timeout;
}

CallMediaPathState CallMediaLegCoordinator::PathState(const CallMediaLegId id) const {
  Impl::CallbackLock lock(*impl_);
  CallMediaPathState state;
  if (const auto* bundle = impl_->FindByLegId(id)) {
    state.active_kind = bundle->active.kind;
    state.active_gen = bundle->active.gen;
    state.candidate = bundle->candidate.has_value();
    state.standby = bundle->standby.has_value();
    state.standby_kind = bundle->standby ? bundle->standby->kind : CallMediaLinkKind::Unknown;
    state.reconnecting = bundle->Reconnecting();
    state.retiring = bundle->retiring.has_value();
  }
  return state;
}

Roe<void> CallMediaLegCoordinator::SendMedia(const CallMediaLegId id, const uint8_t channel,
                                             const std::vector<uint8_t>& payload, const uint32_t seq,
                                             const uint8_t mark) {
  std::shared_ptr<pp::amp::ChannelSession> session;
  CallMediaDirectConnectParams params;
  {
    Impl::CallbackLock lock(*impl_);
    auto* bundle = impl_->FindByLegId(id);
    if (!bundle || bundle->phase != CallMediaBundlePhase::MediaReady || !bundle->active.media) {
      return Error("amp call-media: not in media ready");
    }
    session = bundle->active.media;
    params = bundle->params;
    if (bundle->active.kind == impl_->silenced_kind_for_test.load(std::memory_order_relaxed)) {
      return Roe<void>();  // test: this path went quiet
    }
  }
  auto encrypted =
      EncryptCallMediaFrame(params.media_key, params.call_id, params.media_epoch, seq, mark, channel, payload);
  if (!encrypted) {
    return encrypted.error();
  }
  auto framed = EncodeLengthPrefixedFrame(*encrypted);
  // The channel session is io-affine: the capture thread enqueues under the runtime io lock, or it
  // races the mesh pump on the same session — a link drop orphaning the session cleared its queue
  // mid-push (hard-w5 cold-upgrade answerer SIGSEGV, 2026-09-29). Not under the callback lock: a
  // failed write fails the channel synchronously and its closed callback takes that lock
  // (io lock → callback lock, as the io tick). Same rule as MediaRelayClientCoordinator::SendFrame.
  if (!runtime_.WithIoLock([&]() { return session->EnqueueOutbound(std::move(framed)); })) {
    return Error("amp call-media: send queue full");
  }
  return Roe<void>();
}

Roe<void> CallMediaLegCoordinator::SendAudio(const CallMediaLegId id, const std::vector<uint8_t>& opus_payload,
                                             const uint32_t seq, const uint8_t mark) {
  return SendMedia(id, kMediaChannelAudio, opus_payload, seq, mark);
}

} // namespace pbr
