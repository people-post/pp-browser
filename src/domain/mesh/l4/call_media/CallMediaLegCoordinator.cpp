#include "domain/mesh/l4/call_media/CallMediaLegCoordinator.h"

#include "domain/mesh/l4/shared/ProductChannelPolicies.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/PeerLink.h"
#include "domain/mesh/l4/call_media/CallMediaBundleLogic.h"
#include "domain/mesh/l4/call_media/CallMediaFrameCrypto.h"
#include "domain/mesh/l4/call_media/CallMediaSessionLogic.h"
#include "common/LengthPrefixedCodec.h"

#include "common/ValueJson.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> Utf8Body(const std::string& utf8) {
  return std::vector<uint8_t>(utf8.begin(), utf8.end());
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

void RunWorker(const CallMediaLegCoordinator::WorkerPost& post_worker, std::function<void()> task) {
  if (post_worker) {
    post_worker(std::move(task));
  } else {
    task();
  }
}

/** Parent-owned slot teardown ([A027]): close channel, then unbind mux handlers. */
void CloseQuietSlot(std::shared_ptr<pp::amp::ChannelSession>& slot, pp::amp::PeerLink* link) {
  auto session = std::move(slot);
  if (!session) {
    return;
  }
  // Only call into mux when this session is still bound to a live Connected link's mux.
  // Destroyed ChannelMux (PeerLink drop) has bucket_count==0 → SIGFPE on unordered_map hash%.
  if (link && link->Mux() && link->Phase() == pp::amp::PeerLinkPhase::Connected &&
      session->Mux() == link->Mux()) {
    session->CloseQuiet();
    session->ReleaseHandlers();
  } else {
    session->OrphanFromMux();
  }
}

bool IsPendingCallId(const std::string& call_id) {
  return call_id.rfind("__pending_", 0) == 0;
}

} // namespace

struct CallMediaLegCoordinator::Impl : std::enable_shared_from_this<Impl> {
  struct Bundle {
    CallMediaLegId leg_id{};
    std::string call_id;
    CallMediaBundlePhase phase = CallMediaBundlePhase::Idle;
    bool offerer = false;
    bool local_cancel = false;
    bool finished = true;
    bool control_ready = false;
    bool media_bound = false;

    CallMediaDirectConnectParams params;
    CallMediaDirectCallbacks callbacks;
    LegFinished on_finished;
    Clock::time_point deadline{};

    std::shared_ptr<pp::amp::ChannelSession> outbound_control;
    std::shared_ptr<pp::amp::ChannelSession> inbound_control;
    std::shared_ptr<pp::amp::ChannelSession> media;
  };

  pp::amp::MeshRuntime* runtime = nullptr;
  WorkerPost post_worker;
  mutable std::mutex mu;
  InboundHandler inbound;
  std::atomic<bool> stopped{false};
  std::atomic<bool> started{false};
  std::atomic<uint64_t> next_leg_id{1};
  pp::amp::MeshRuntime::IoTickId io_tick_id = 0;

  /** call_id → bundle */
  std::unordered_map<std::string, std::unique_ptr<Bundle>> bundles;
  /** channel_id → (call_id, role) for close/frame ownership checks */
  std::unordered_map<uint32_t, std::pair<std::string, CallMediaChannelRole>> channel_index;

  void PostIo(std::function<void()> task) {
    if (!runtime || stopped.load(std::memory_order_acquire) || !task) {
      return;
    }
    runtime->PostToIo(std::move(task));
  }

  bool LocalWinsForLink(const pp::amp::PeerLink& link) const {
    if (!runtime) {
      return true;
    }
    std::string remote = link.RemotePeerId();
    // Inbound links often have an empty RemotePeerId until handshake/rekey. Prefer the
    // dialed alias link (same peer_key after rekey, or StartLeg peer_key) so glare stays
    // antisymmetric — empty remote previously made *both* sides RejectGlare.
    if (remote.empty() && !link.PeerKey().empty()) {
      if (const auto* known = runtime->Links().FindLink(link.PeerKey())) {
        remote = known->RemotePeerId();
      }
    }
    return LocalWinsCallMediaGlare(runtime->Links().LocalPeerId(), remote);
  }

  bool LocalWinsForBundle(const Bundle& bundle, const pp::amp::PeerLink& fallback_link) const {
    if (!bundle.params.peer_key.empty()) {
      if (const auto* dial = runtime->Links().FindLink(bundle.params.peer_key)) {
        return LocalWinsForLink(*dial);
      }
    }
    return LocalWinsForLink(fallback_link);
  }

  pp::amp::PeerLink* ResolveLink(const Bundle& bundle) const {
    if (!runtime || bundle.params.peer_key.empty()) {
      return nullptr;
    }
    return runtime->Links().FindLink(bundle.params.peer_key);
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

  bool OwnsRole(const Bundle& bundle, const CallMediaChannelRole role,
                const pp::amp::ChannelSession* session) const {
    if (!session) {
      return false;
    }
    switch (role) {
    case CallMediaChannelRole::OutboundControl:
      return bundle.outbound_control.get() == session;
    case CallMediaChannelRole::InboundControl:
      return bundle.inbound_control.get() == session;
    case CallMediaChannelRole::Media:
      return bundle.media.get() == session;
    }
    return false;
  }

  /** True when CloseQuiet/ReleaseHandlers may safely touch the session's mux. */
  bool MuxAliveForBundle(const Bundle& bundle) const {
    pp::amp::PeerLink* link = ResolveLink(bundle);
    return link && link->Mux() && link->Phase() == pp::amp::PeerLinkPhase::Connected;
  }

  /** PeerLink erased (DropLink) — ChannelSession mux_ may already be dangling. */
  bool PeerLinkMissing(const Bundle& bundle) const {
    if (!runtime || bundle.params.peer_key.empty()) {
      return false;
    }
    return runtime->Links().FindLink(bundle.params.peer_key) == nullptr;
  }

  void DropRole(Bundle& bundle, const CallMediaChannelRole role) {
    pp::amp::PeerLink* link = ResolveLink(bundle);
    switch (role) {
    case CallMediaChannelRole::OutboundControl:
      if (bundle.outbound_control) {
        UnindexChannel(bundle.outbound_control->ChannelId());
      }
      CloseQuietSlot(bundle.outbound_control, link);
      break;
    case CallMediaChannelRole::InboundControl:
      if (bundle.inbound_control) {
        UnindexChannel(bundle.inbound_control->ChannelId());
      }
      CloseQuietSlot(bundle.inbound_control, link);
      break;
    case CallMediaChannelRole::Media:
      if (bundle.media) {
        UnindexChannel(bundle.media->ChannelId());
      }
      CloseQuietSlot(bundle.media, link);
      bundle.media_bound = false;
      break;
    }
  }

  void FinishBundle(Bundle& bundle, Roe<void> result) {
    if (bundle.finished) {
      return;
    }
    bundle.finished = true;
    LegFinished cb = std::move(bundle.on_finished);
    bundle.on_finished = {};
    if (cb) {
      cb(std::move(result));
    }
  }

  void EraseBundle(const std::string& call_id) {
    auto* bundle = FindByCallId(call_id);
    if (!bundle) {
      return;
    }
    DropRole(*bundle, CallMediaChannelRole::OutboundControl);
    DropRole(*bundle, CallMediaChannelRole::InboundControl);
    DropRole(*bundle, CallMediaChannelRole::Media);
    bundles.erase(call_id);
  }

  void TearDownBundle(Bundle& bundle, const bool finish_with_abort, const bool notify_failed,
                      const std::string& fail_message) {
    const std::string call_id = bundle.call_id;
    bundle.local_cancel = bundle.local_cancel || finish_with_abort;
    bundle.phase = CallMediaBundlePhase::Closing;
    if (!bundle.finished) {
      if (finish_with_abort) {
        FinishBundle(bundle, Error("call-media aborted"));
      } else if (notify_failed) {
        if (bundle.callbacks.on_failed && !fail_message.empty()) {
          bundle.callbacks.on_failed(fail_message);
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
    if (!bundle.control_ready || !bundle.media_bound) {
      return;
    }
    bundle.phase = CallMediaBundlePhase::MediaReady;
    CallMediaDirectCallbacks cbs = bundle.callbacks;
    if (cbs.on_connected) {
      cbs.on_connected();
    }
    FinishBundle(bundle, {});
  }

  void TryEnterMediaReady(Bundle& bundle) { EnterMediaReady(bundle); }

  void ScheduleWhenChannelOpen(const std::string& peer_key, const uint32_t channel_id,
                               const Clock::time_point deadline, std::function<void(bool open)> done) {
    PostIo([this, self = shared_from_this(), peer_key, channel_id, deadline, done = std::move(done)]() mutable {
      if (stopped.load(std::memory_order_acquire)) {
        done(false);
        return;
      }
      pp::amp::PeerLink* link = runtime ? runtime->Links().FindLink(peer_key) : nullptr;
      if (!link || !link->Mux()) {
        done(false);
        return;
      }
      if (link->Mux()->State(channel_id) == pp::amp::ChannelState::Open) {
        done(true);
        return;
      }
      if (Clock::now() >= deadline) {
        done(false);
        return;
      }
      ScheduleWhenChannelOpen(peer_key, channel_id, deadline, std::move(done));
    });
  }

  void TickDeadlines() {
    const auto now = Clock::now();
    std::vector<std::string> timed_out;
    std::vector<std::string> link_lost;
    {
      std::lock_guard lock(mu);
      for (auto& [call_id, bundle] : bundles) {
        if (!bundle || IsPendingCallId(call_id)) {
          continue;
        }
        if (bundle->phase == CallMediaBundlePhase::Idle || bundle->phase == CallMediaBundlePhase::Closing) {
          continue;
        }
        // PeerLink drop leaves ChannelSession mux_ dangling — tear down before L4 touches it.
        // Do not treat Handshaking as lost (FindLink still present until DropLink).
        if (PeerLinkMissing(*bundle)) {
          link_lost.push_back(call_id);
          continue;
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
    for (const auto& call_id : link_lost) {
      std::lock_guard lock(mu);
      auto* bundle = FindByCallId(call_id);
      if (!bundle || bundle->phase == CallMediaBundlePhase::Idle ||
          bundle->phase == CallMediaBundlePhase::Closing) {
        continue;
      }
      TearDownBundle(*bundle, /*finish_with_abort=*/false, /*notify_failed=*/true,
                     "amp call-media: peer link lost");
    }
    for (const auto& call_id : timed_out) {
      std::lock_guard lock(mu);
      auto* bundle = FindByCallId(call_id);
      if (!bundle || bundle->finished) {
        continue;
      }
      TearDownBundle(*bundle, /*finish_with_abort=*/false, /*notify_failed=*/false,
                     "amp call-media connect timed out");
    }
  }

  void OnChannelClosed(const std::string& /*call_id*/, const CallMediaChannelRole role,
                       const std::shared_ptr<pp::amp::ChannelSession>& session, const char* reason) {
    std::lock_guard lock(mu);
    Bundle* bundle = nullptr;
    for (auto& [_, b] : bundles) {
      if (b && OwnsRole(*b, role, session.get())) {
        bundle = b.get();
        break;
      }
    }
    if (!bundle) {
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
    auto channel_id = link->Mux()->OpenOutbound(kCallMediaDirectProtocolId, pp::amp::CallMediaChannelPolicy());
    if (!channel_id) {
      TearDownBundle(bundle, false, false, channel_id.error().message);
      return;
    }
    const auto call_id = bundle.call_id;
    const auto leg_id = bundle.leg_id;
    const auto deadline = bundle.deadline;
    const std::string peer_key = bundle.params.peer_key;
    ScheduleWhenChannelOpen(peer_key, *channel_id, deadline,
                            [this, self = shared_from_this(), call_id, leg_id, peer_key,
                             channel_id = *channel_id](const bool open) {
                              std::lock_guard lock(mu);
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
                              auto* resolved = runtime->Links().FindLink(peer_key);
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
    if (bundle.params.peer_key.empty()) {
      bundle.params.peer_key = link.PeerKey();
    }
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
          OnChannelClosed(call_id, role, channel_session, reason);
        });
    IndexChannel(channel_id, call_id, role);
    if (role == CallMediaChannelRole::OutboundControl) {
      bundle.outbound_control = std::move(channel_session);
    } else {
      bundle.inbound_control = std::move(channel_session);
    }
  }

  void BindMediaChannel(Bundle& bundle, pp::amp::PeerLink& link, const uint32_t channel_id) {
    if (bundle.params.peer_key.empty()) {
      bundle.params.peer_key = link.PeerKey();
    }
    auto channel_session = std::make_shared<pp::amp::ChannelSession>();
    const std::string call_id = bundle.call_id;
    channel_session->Bind(
        *link.Mux(), channel_id, pp::amp::CallMediaChannelPolicy(),
        [this, self = shared_from_this(), call_id](Roe<std::vector<uint8_t>> frame) {
          if (!frame) {
            return false;
          }
          return HandleMediaBody(call_id, *frame);
        },
        [this, self = shared_from_this(), call_id, channel_session](const char* reason) {
          OnChannelClosed(call_id, CallMediaChannelRole::Media, channel_session, reason);
        });
    IndexChannel(channel_id, call_id, CallMediaChannelRole::Media);
    bundle.media = std::move(channel_session);
    bundle.media_bound = true;
  }

  bool HandleMediaBody(const std::string& call_id, const std::vector<uint8_t>& frame) {
    CallMediaDirectCallbacks cbs;
    CallMediaDirectConnectParams params;
    {
      std::lock_guard lock(mu);
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
    if (cbs.on_media) {
      cbs.on_media(decoded->channel, decoded->payload);
    } else if (decoded->channel == kCallMediaChannelAudio && cbs.on_audio) {
      cbs.on_audio(decoded->payload);
    }
    return true;
  }


  Bundle* FindByInboundSession(const std::shared_ptr<pp::amp::ChannelSession>& session) {
    for (auto& [_, bundle] : bundles) {
      if (bundle && bundle->inbound_control == session) {
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
    if (node.mapped()->inbound_control) {
      IndexChannel(node.mapped()->inbound_control->ChannelId(), call_id, CallMediaChannelRole::InboundControl);
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
    if (type == "hello") {
      HandleInboundHello(channel_session, *parsed);
      return;
    }
    if (type == "hello_ack") {
      HandleHelloAck(known_call_id, role, *parsed);
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
      std::lock_guard lock(mu);
      Bundle* holder = FindByInboundSession(channel_session);
      if (!holder) {
        return;
      }
      link = ResolveLink(*holder);
      if (!link) {
        return;
      }

      Bundle* target = FindByCallId(hello_call_id);
      if (IsPendingCallId(holder->call_id)) {
        if (target && target != holder) {
          DropRole(*target, CallMediaChannelRole::InboundControl);
          target->inbound_control = std::move(holder->inbound_control);
          if (target->params.peer_key.empty()) {
            target->params.peer_key = link->PeerKey();
          }
          if (target->inbound_control) {
            IndexChannel(target->inbound_control->ChannelId(), hello_call_id, CallMediaChannelRole::InboundControl);
          }
          EraseBundle(holder->call_id);
          holder = target;
        } else {
          RekeyPendingToCallId(*holder, hello_call_id);
          target = FindByCallId(hello_call_id);
        }
      } else {
        target = holder;
      }
      if (!target) {
        return;
      }

      CallMediaInboundHelloContext ctx;
      ctx.phase = target->phase;
      ctx.has_outbound_control = static_cast<bool>(target->outbound_control);
      ctx.offerer = target->offerer;
      ctx.local_wins_glare = LocalWinsForBundle(*target, *link);
      ctx.other_bundle_busy = OtherBundleBusy(hello_call_id);
      const auto decision = DecideCallMediaInboundHello(ctx);

      if (decision == CallMediaInboundHelloDecision::RejectBusy) {
        (void)channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(false, "busy")));
        DropRole(*target, CallMediaChannelRole::InboundControl);
        if (!target->outbound_control && target->phase == CallMediaBundlePhase::Idle) {
          EraseBundle(target->call_id);
        }
        return;
      }
      if (decision == CallMediaInboundHelloDecision::RejectGlare) {
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
    }

    CallMediaDirectConnectParams params;
    params.call_id = hello_call_id;
    params.media_epoch = static_cast<uint32_t>(hello.getNonNegInt("media_epoch").value_or(1));
    params.offerer = hello.getString("role").value_or("") == "offerer";
    params.peer_key = link->PeerKey();

    InboundHandler handler;
    {
      std::lock_guard lock(mu);
      handler = inbound;
    }
    if (!handler) {
      (void)channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(false, "no handler")));
      std::lock_guard lock(mu);
      auto* bundle = FindByCallId(hello_call_id);
      if (bundle) {
        DropRole(*bundle, CallMediaChannelRole::InboundControl);
        if (!bundle->outbound_control) {
          EraseBundle(hello_call_id);
        } else {
          bundle->phase = CallMediaBundlePhase::OutboundHello;
        }
      }
      return;
    }

    const std::string peer_key = link->PeerKey();
    RunWorker(post_worker, [this, self = shared_from_this(), channel_session, params, handler = std::move(handler),
                            peer_key, call_id = hello_call_id]() mutable {
      CallMediaDirectConnectParams answer_params = params;
      CallMediaDirectCallbacks answer_cbs;
      handler(answer_params, answer_cbs);
      PostIo([this, self, channel_session, answer_params = std::move(answer_params),
              answer_cbs = std::move(answer_cbs), peer_key, call_id]() mutable {
        std::lock_guard lock(mu);
        auto* bundle = FindByCallId(call_id);
        if (!bundle || bundle->phase != CallMediaBundlePhase::InboundHello) {
          return;
        }
        pp::amp::PeerLink* resolved = runtime->Links().FindLink(peer_key);
        if (!resolved) {
          return;
        }
        if (bundle->offerer && LocalWinsForBundle(*bundle, *resolved) && bundle->outbound_control) {
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
        if (bundle->params.peer_key.empty()) {
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
    });
  }

  void HandleHelloAck(const std::string& call_id, const CallMediaChannelRole role, const Object& ack) {
    std::lock_guard lock(mu);
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
      if (bundle->inbound_control) {
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
      std::lock_guard lock(mu);
      auto pending = std::make_unique<Bundle>();
      pending->call_id = std::string("__pending_") + std::to_string(channel_id);
      pending->leg_id = CallMediaLegId{next_leg_id.fetch_add(1, std::memory_order_relaxed)};
      pending->params.peer_key = link.PeerKey();
      pending->deadline = Clock::now() + std::chrono::seconds(15);
      pending->finished = true;
      pending->phase = CallMediaBundlePhase::Idle;
      const std::string pending_id = pending->call_id;
      auto* raw = pending.get();
      bundles.emplace(pending_id, std::move(pending));
      BindControlChannel(*raw, link, channel_id, CallMediaChannelRole::InboundControl);
      return;
    }
    if (cls == pp::amp::ChannelClass::Realtime) {
      std::lock_guard lock(mu);
      Bundle* target = nullptr;
      for (auto& [_, bundle] : bundles) {
        if (bundle && BundleMatchesLink(*bundle, link) && bundle->phase == CallMediaBundlePhase::AwaitingMedia &&
            !bundle->media_bound) {
          target = bundle.get();
          break;
        }
      }
      if (!target) {
        for (auto& [_, bundle] : bundles) {
          if (bundle && BundleMatchesLink(*bundle, link) && bundle->control_ready && !bundle->media_bound) {
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
  bool AdoptOutboundIntoExisting(Bundle& existing, const CallMediaLegId leg_id,
                                 CallMediaDirectCallbacks& callbacks, LegFinished& on_finished,
                                 const int timeout_ms) {
    const auto phase = existing.phase;
    if (phase != CallMediaBundlePhase::InboundHello && phase != CallMediaBundlePhase::AwaitingMedia &&
        phase != CallMediaBundlePhase::MediaReady) {
      return false;
    }
    existing.leg_id = leg_id;
    existing.deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 15000);
    // Inbound worker may not have installed answer callbacks yet.
    if (!existing.callbacks.on_audio && !existing.callbacks.on_media && !existing.callbacks.on_connected) {
      existing.callbacks = std::move(callbacks);
    }
    if (existing.finished) {
      // MediaReady (or failed) settled before dialer callback was attached.
      LegFinished cb = std::move(on_finished);
      if (cb) {
        if (phase == CallMediaBundlePhase::MediaReady) {
          cb({});
        } else {
          cb(Error("call-media aborted"));
        }
      }
      return true;
    }
    existing.on_finished = std::move(on_finished);
    return true;
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
      std::lock_guard lock(mu);
      if (auto* existing = FindByCallId(params.call_id)) {
        if (AdoptOutboundIntoExisting(*existing, leg_id, callbacks, on_finished, timeout_ms)) {
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

    auto open_control = std::make_shared<std::function<void(int)>>();
    *open_control = [this, self = shared_from_this(), leg_id, peer_key, call_id, params, deadline,
                     open_control](const int retries) {
      runtime->Links().OpenChannel(
          peer_key, kCallMediaDirectProtocolId, pp::amp::CallMediaControlChannelPolicy(),
          [this, self, leg_id, peer_key, call_id, params, deadline, retries,
           open_control](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
            std::unique_lock lock(mu);
            auto* bundle = FindByCallId(call_id);
            if (!bundle || bundle->leg_id.value != leg_id.value) {
              return;
            }
            if (!channel) {
              if (pp::amp::PeerLinkManager::IsAssociationNotReady(channel.error()) && retries < 500
                  && !bundle->finished) {
                lock.unlock();
                PostIo([open_control, retries]() { (*open_control)(retries + 1); });
                return;
              }
              if (bundle->phase == CallMediaBundlePhase::OutboundHello) {
                TearDownBundle(*bundle, false, false, channel.error().message);
              }
              return;
            }
            auto* link = runtime->Links().FindLink(peer_key);
            if (!link) {
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
            lock.unlock();
            ScheduleWhenChannelOpen(peer_key, *channel, deadline,
                                    [this, self, leg_id, call_id, peer_key, channel = *channel,
                                     params](const bool open) {
                                      std::lock_guard lock(mu);
                                      auto* bundle = FindByCallId(call_id);
                                      if (!bundle || bundle->leg_id.value != leg_id.value) {
                                        return;
                                      }
                                      auto* link = runtime->Links().FindLink(peer_key);
                                      if (!open) {
                                        if (bundle->phase == CallMediaBundlePhase::OutboundHello) {
                                          TearDownBundle(*bundle, false, false,
                                                         "amp call-media: channel open failed");
                                        } else if (link && link->Mux()) {
                                          (void)link->Mux()->CloseChannel(channel, "call-media glare yield");
                                        }
                                        return;
                                      }
                                      if (bundle->phase != CallMediaBundlePhase::OutboundHello) {
                                        if (link && link->Mux()) {
                                          (void)link->Mux()->CloseChannel(channel, "call-media glare yield");
                                        }
                                        return;
                                      }
                                      if (!link) {
                                        TearDownBundle(*bundle, false, false, "amp call-media: peer link missing");
                                        return;
                                      }
                                      BindControlChannel(*bundle, *link, channel, CallMediaChannelRole::OutboundControl);
                                      if (!bundle->outbound_control ||
                                          !bundle->outbound_control->EnqueueOutbound(Utf8Body(BuildHelloJson(params)))) {
                                        TearDownBundle(*bundle, false, false, "amp call-media: hello write failed");
                                      }
                                    });
          });
    };
    (*open_control)(0);
  }
};

CallMediaLegCoordinator::CallMediaLegCoordinator(pp::amp::MeshRuntime& runtime, WorkerPost post_worker)
    : impl_(std::make_shared<Impl>()), runtime_(runtime) {
  impl_->runtime = &runtime_;
  impl_->post_worker = std::move(post_worker);
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
  runtime_.Links().SetProtocolHandler(kCallMediaDirectProtocolId,
                                      [weak = std::weak_ptr(impl_)](pp::amp::PeerLink& link, const uint32_t ch) {
                                        if (auto impl = weak.lock()) {
                                          impl->HandleInboundChannel(link, ch);
                                        }
                                      });
}

void CallMediaLegCoordinator::Stop() {
  impl_->started.store(false, std::memory_order_release);
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.RemoveIoTick(impl_->io_tick_id);
  impl_->io_tick_id = 0;
  runtime_.Links().RemoveProtocolHandler(kCallMediaDirectProtocolId);
  // Tear down synchronously: a PostIo(raw Impl*) races if the caller destroys then Pumps
  // (macOS: "mutex lock failed: Invalid argument").
  {
    std::lock_guard lock(impl_->mu);
    std::vector<std::string> ids;
    for (auto& [id, _] : impl_->bundles) {
      ids.push_back(id);
    }
    for (const auto& id : ids) {
      if (auto* b = impl_->FindByCallId(id)) {
        impl_->TearDownBundle(*b, true, false, "call-media aborted");
      }
    }
  }
  ClearInboundHandler();
  // Drop runtime before callers destroy MeshRuntime / harness (detached WorkerPost may resume).
  impl_->runtime = nullptr;
}

void CallMediaLegCoordinator::SetInboundHandler(InboundHandler handler) {
  std::lock_guard lock(impl_->mu);
  impl_->inbound = std::move(handler);
}

void CallMediaLegCoordinator::ClearInboundHandler() {
  std::lock_guard lock(impl_->mu);
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
    std::lock_guard lock(impl->mu);
    if (auto* b = impl->FindByLegId(id)) {
      impl->TearDownBundle(*b, true, false, "call-media aborted");
    }
  });
}

void CallMediaLegCoordinator::DetachLeg(const CallMediaLegId id) {
  impl_->PostIo([impl = impl_, id]() {
    std::lock_guard lock(impl->mu);
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
  std::lock_guard lock(impl_->mu);
  const auto* bundle = impl_->FindByLegId(id);
  return bundle && CallMediaBundlePhaseIsActive(bundle->phase);
}

bool CallMediaLegCoordinator::IsActive() const {
  std::lock_guard lock(impl_->mu);
  const auto* bundle = impl_->PrimaryBundle();
  return bundle && CallMediaBundlePhaseIsActive(bundle->phase);
}

CallMediaLegId CallMediaLegCoordinator::PrimaryLegId() const {
  std::lock_guard lock(impl_->mu);
  const auto* bundle = impl_->PrimaryBundle();
  return bundle ? bundle->leg_id : CallMediaLegId{};
}

CallMediaDirectConnectParams CallMediaLegCoordinator::ActiveParams() const {
  std::lock_guard lock(impl_->mu);
  const auto* bundle = impl_->PrimaryBundle();
  return bundle ? bundle->params : CallMediaDirectConnectParams{};
}

CallMediaLegPhase CallMediaLegCoordinator::LegPhase(const CallMediaLegId id) const {
  if (!id) {
    return CallMediaLegPhase::Closed;
  }
  std::lock_guard lock(impl_->mu);
  const auto* bundle = impl_->FindByLegId(id);
  if (!bundle) {
    return CallMediaLegPhase::Closed;
  }
  return CallMediaBundlePhaseToLegPhase(bundle->phase);
}

CallMediaSessionPhase CallMediaLegCoordinator::Phase() const {
  std::lock_guard lock(impl_->mu);
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
  std::lock_guard lock(impl_->mu);
  const auto* bundle = impl_->FindByLegId(id);
  return bundle ? bundle->phase : CallMediaBundlePhase::Idle;
}

Roe<void> CallMediaLegCoordinator::SendMedia(const CallMediaLegId id, const uint8_t channel,
                                             const std::vector<uint8_t>& payload, const uint32_t seq,
                                             const uint8_t mark) {
  std::shared_ptr<pp::amp::ChannelSession> session;
  CallMediaDirectConnectParams params;
  {
    std::lock_guard lock(impl_->mu);
    auto* bundle = impl_->FindByLegId(id);
    if (!bundle || bundle->phase != CallMediaBundlePhase::MediaReady || !bundle->media) {
      return Error("amp call-media: not in media ready");
    }
    session = bundle->media;
    params = bundle->params;
  }
  auto encrypted =
      EncryptCallMediaFrame(params.media_key, params.call_id, params.media_epoch, seq, mark, channel, payload);
  if (!encrypted) {
    return encrypted.error();
  }
  auto framed = EncodeLengthPrefixedFrame(*encrypted);
  if (!session->EnqueueOutbound(std::move(framed))) {
    return Error("amp call-media: send queue full");
  }
  return Roe<void>();
}

Roe<void> CallMediaLegCoordinator::SendAudio(const CallMediaLegId id, const std::vector<uint8_t>& opus_payload,
                                             const uint32_t seq, const uint8_t mark) {
  return SendMedia(id, kCallMediaChannelAudio, opus_payload, seq, mark);
}

} // namespace pbr
