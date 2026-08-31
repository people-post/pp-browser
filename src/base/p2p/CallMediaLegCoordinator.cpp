#include "base/p2p/CallMediaLegCoordinator.h"

#include "base/mesh/channel/ChannelPolicy.h"
#include "base/mesh/channel/ChannelSession.h"
#include "base/p2p/CallMediaFrameCrypto.h"
#include "base/p2p/CallMediaSessionLogic.h"
#include "base/p2p/StreamFrameIo.h"

#include "common/ValueJson.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
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

} // namespace

struct CallMediaLegCoordinator::Impl {
  amp::MeshRuntime* runtime = nullptr;
  WorkerPost post_worker;
  std::mutex mu;
  InboundHandler inbound;
  std::atomic<bool> stopped{false};
  std::atomic<bool> started{false};

  std::atomic<uint64_t> next_leg_id{1};
  CallMediaLegId active_leg_id{};
  std::atomic<CallMediaLegPhase> leg_phase{CallMediaLegPhase::Closed};
  std::atomic<CallMediaSessionPhase> phase{CallMediaSessionPhase::Idle};

  CallMediaDirectConnectParams active_params;
  CallMediaDirectCallbacks callbacks;
  LegFinished leg_finished;
  std::atomic<bool> leg_finished_settled{true};
  std::atomic<bool> local_cancel{false};
  bool offerer_glare = false;

  amp::PeerLink* active_link = nullptr;
  std::shared_ptr<amp::ChannelSession> outbound_control_session;
  std::shared_ptr<amp::ChannelSession> control_session;
  std::shared_ptr<amp::ChannelSession> media_session;
  bool ignore_control_close = false;
  std::atomic<bool> control_ready{false};
  std::atomic<bool> media_bound{false};
  Clock::time_point connect_deadline{};

  void MaybeFireConnectDeadline() {
    if (connect_deadline.time_since_epoch().count() == 0) {
      return;
    }
    if (leg_finished_settled.load(std::memory_order_acquire)) {
      return;
    }
    if (Clock::now() < connect_deadline) {
      return;
    }
    const CallMediaLegId leg_id = active_leg_id;
    connect_deadline = {};
    FailLeg(Error("amp call-media connect timed out"), /*remote_terminal=*/false);
    TeardownLeg(leg_id, /*finish_with_abort=*/false);
  }

  void TickConnectDeadline() { MaybeFireConnectDeadline(); }

  void PostIo(std::function<void()> task) {
    if (!runtime) {
      return;
    }
    runtime->PostToIo([this, task = std::move(task)]() mutable {
      MaybeFireConnectDeadline();
      if (task) {
        task();
      }
    });
  }

  void ScheduleWhenChannelOpen(amp::PeerLink* link, const uint32_t channel_id, const Clock::time_point deadline,
                               std::function<void(bool open)> done) {
    PostIo([this, link, channel_id, deadline, done = std::move(done)]() mutable {
      if (stopped.load(std::memory_order_acquire)) {
        done(false);
        return;
      }
      if (!link || !link->Mux()) {
        done(false);
        return;
      }
      if (link->Mux()->State(channel_id) == amp::ChannelState::Open) {
        done(true);
        return;
      }
      if (Clock::now() >= deadline) {
        done(false);
        return;
      }
      ScheduleWhenChannelOpen(link, channel_id, deadline, std::move(done));
    });
  }

  void ResetFieldsUnlocked() {
    active_link = nullptr;
    callbacks = {};
    active_params = {};
    leg_phase.store(CallMediaLegPhase::Closed, std::memory_order_release);
    phase.store(CallMediaSessionPhase::Idle, std::memory_order_release);
    control_ready.store(false, std::memory_order_release);
    media_bound.store(false, std::memory_order_release);
    offerer_glare = false;
    local_cancel.store(false, std::memory_order_release);
    leg_finished = {};
    active_leg_id = {};
    connect_deadline = {};
    outbound_control_session.reset();
  }

  void ResetLocked() {
    CloseControlQuiet(control_session);
    CloseControlQuiet(outbound_control_session);
    control_session.reset();
    outbound_control_session.reset();
    CloseControlQuiet(media_session);
    media_session.reset();
    ResetFieldsUnlocked();
  }

  void CloseControlQuiet(const std::shared_ptr<amp::ChannelSession>& session) {
    if (!session || session->IsClosed()) {
      return;
    }
    ignore_control_close = true;
    session->Close();
    ignore_control_close = false;
  }

  void FinishLeg(Roe<void> result) {
    LegFinished cb;
    {
      std::lock_guard lock(mu);
      if (leg_finished_settled.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      cb = std::move(leg_finished);
      leg_finished = {};
    }
    if (cb) {
      cb(std::move(result));
    }
  }

  bool LocalWinsGlareForLink(const amp::PeerLink& link) const {
    if (!runtime) {
      return true;
    }
    return LocalWinsCallMediaGlare(runtime->Links().LocalPeerId(), link.RemotePeerId());
  }

  void EnterMediaReady() {
    leg_phase.store(CallMediaLegPhase::MediaReady, std::memory_order_release);
    phase.store(CallMediaSessionPhase::MediaReady, std::memory_order_release);
    CallMediaDirectCallbacks cbs;
    {
      std::lock_guard lock(mu);
      cbs = callbacks;
    }
    if (cbs.on_connected) {
      cbs.on_connected();
    }
    FinishLeg({});
  }

  void TryEnterMediaReady() {
    if (!control_ready.load(std::memory_order_acquire) || !media_bound.load(std::memory_order_acquire)) {
      return;
    }
    if (leg_phase.load(std::memory_order_acquire) == CallMediaLegPhase::MediaReady) {
      return;
    }
    EnterMediaReady();
  }

  void AbandonOutbound() {
    std::shared_ptr<amp::ChannelSession> outbound;
    {
      std::lock_guard lock(mu);
      offerer_glare = false;
      outbound = std::move(outbound_control_session);
      if (control_session == outbound) {
        control_session.reset();
      }
      leg_phase.store(CallMediaLegPhase::Closed, std::memory_order_release);
      phase.store(CallMediaSessionPhase::Dialing, std::memory_order_release);
    }
    CloseControlQuiet(outbound);
  }

  void FailLeg(Roe<void> result, const bool remote_terminal) {
    CallMediaDirectCallbacks cbs;
    CallMediaSessionPhase session_phase = CallMediaSessionPhase::Idle;
    {
      std::lock_guard lock(mu);
      session_phase = phase.load(std::memory_order_acquire);
      cbs = callbacks;
      if (session_phase != CallMediaSessionPhase::Idle) {
        phase.store(CallMediaSessionPhase::Detaching, std::memory_order_release);
      }
    }
    const bool suppress_notify =
        local_cancel.load(std::memory_order_acquire) || CallMediaFailNotifySuppressed(session_phase);
    if (remote_terminal && !suppress_notify && cbs.on_failed && result) {
      cbs.on_failed(result.error().message);
    }
    FinishLeg(std::move(result));
  }

  void TeardownLeg(const CallMediaLegId leg_id, const bool finish_with_abort) {
    if (leg_id.value != 0 && active_leg_id.value != leg_id.value) {
      return;
    }
    const bool connect_pending = !leg_finished_settled.load(std::memory_order_acquire);
    local_cancel.store(true, std::memory_order_release);
    if (connect_pending && finish_with_abort) {
      FinishLeg(Error("call-media aborted"));
    }
    std::shared_ptr<amp::ChannelSession> control;
    std::shared_ptr<amp::ChannelSession> outbound_control;
    std::shared_ptr<amp::ChannelSession> media;
    {
      std::lock_guard lock(mu);
      control = std::move(control_session);
      outbound_control = std::move(outbound_control_session);
      media = std::move(media_session);
      ResetFieldsUnlocked();
    }
    CloseControlQuiet(control);
    CloseControlQuiet(outbound_control);
    CloseControlQuiet(media);
  }

  void OnChannelClosed(const char* reason) {
    if (ignore_control_close) {
      return;
    }
    CallMediaDirectCallbacks cbs;
    CallMediaSessionPhase session_phase = CallMediaSessionPhase::Idle;
    bool should_abandon_outbound = false;
    bool connect_pending = false;
    {
      std::lock_guard lock(mu);
      session_phase = phase.load(std::memory_order_acquire);
      if (session_phase == CallMediaSessionPhase::Idle) {
        return;
      }
      const bool remote = IsRemoteTerminalReason(reason);
      connect_pending = !leg_finished_settled.load(std::memory_order_acquire);
      cbs = callbacks;
      if (remote && session_phase == CallMediaSessionPhase::HelloOutbound && offerer_glare && active_link &&
          !LocalWinsGlareForLink(*active_link) && !local_cancel.load(std::memory_order_acquire)) {
        should_abandon_outbound = true;
      } else {
        control_session.reset();
        media_session.reset();
        ResetFieldsUnlocked();
      }
    }
    if (should_abandon_outbound) {
      AbandonOutbound();
      return;
    }
    const bool remote = IsRemoteTerminalReason(reason);
    if (remote && !local_cancel.load(std::memory_order_acquire) && !CallMediaFailNotifySuppressed(session_phase)) {
      const std::string message =
          std::string("call-media stream closed (") + (reason && reason[0] ? reason : "unknown") + ")";
      if (cbs.on_failed) {
        cbs.on_failed(message);
      }
      FinishLeg(Error(message));
      return;
    }
    if (connect_pending) {
      FinishLeg(Error("call-media stream closed"));
    }
  }

  void OpenMediaChannelOnActiveLink(const CallMediaLegId leg_id, const Clock::time_point deadline) {
    amp::PeerLink* link = nullptr;
    {
      std::lock_guard lock(mu);
      if (active_leg_id.value != leg_id.value) {
        return;
      }
      link = active_link;
    }
    if (!link || !link->Mux()) {
      FailLeg(Error("amp call-media: no link for media channel"), /*remote_terminal=*/false);
      TeardownLeg(leg_id, /*finish_with_abort=*/false);
      return;
    }
    auto channel_id = link->Mux()->OpenOutbound(kCallMediaDirectProtocolId, amp::CallMediaChannelPolicy());
    if (!channel_id) {
      FailLeg(channel_id.error(), /*remote_terminal=*/false);
      TeardownLeg(leg_id, /*finish_with_abort=*/false);
      return;
    }
    ScheduleWhenChannelOpen(link, *channel_id, deadline, [this, leg_id, link, channel_id = *channel_id,
                                                            deadline](const bool open) mutable {
      if (!open || active_leg_id.value != leg_id.value) {
        FailLeg(Error("amp call-media: media channel open failed"), /*remote_terminal=*/false);
        TeardownLeg(leg_id, /*finish_with_abort=*/false);
        return;
      }
      BindMediaChannel(*link, channel_id);
      TryEnterMediaReady();
    });
  }

  bool HandleControlJson(const std::string& json_utf8, const std::shared_ptr<amp::ChannelSession>& channel_session,
                         amp::PeerLink& link, const Clock::time_point deadline) {
    auto parsed = ParseJsonObject(json_utf8);
    if (!parsed) {
      return false;
    }
    const auto type = parsed->getString("type").value_or("");
    if (type == "hello") {
      bool abandon_outbound = false;
      {
        std::lock_guard lock(mu);
        if (phase.load(std::memory_order_acquire) == CallMediaSessionPhase::MediaReady ||
            leg_phase.load(std::memory_order_acquire) == CallMediaLegPhase::MediaReady) {
          (void)channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(false, "busy")));
          return false;
        }
        if (phase.load(std::memory_order_acquire) == CallMediaSessionPhase::HelloInbound) {
          (void)channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(false, "busy")));
          return false;
        }
        if (phase.load(std::memory_order_acquire) == CallMediaSessionPhase::HelloOutbound && offerer_glare &&
            LocalWinsGlareForLink(link)) {
          (void)channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(false, "glare")));
          CloseControlQuiet(channel_session);
          {
            std::lock_guard lock(mu);
            if (control_session == channel_session) {
              control_session = outbound_control_session;
            }
          }
          return false;
        }
        if (phase.load(std::memory_order_acquire) == CallMediaSessionPhase::HelloOutbound &&
            !LocalWinsGlareForLink(link)) {
          abandon_outbound = true;
        }
      }
      if (abandon_outbound) {
        AbandonOutbound();
      }
      {
        std::lock_guard lock(mu);
        phase.store(CallMediaSessionPhase::HelloInbound, std::memory_order_release);
        leg_phase.store(CallMediaLegPhase::ControlHello, std::memory_order_release);
        active_link = &link;
      }

      CallMediaDirectConnectParams params;
      params.call_id = parsed->getString("call_id").value_or("");
      params.media_epoch = static_cast<uint32_t>(parsed->getNonNegInt("media_epoch").value_or(1));
      params.offerer = parsed->getString("role").value_or("") == "offerer";
      params.peer_key = link.PeerKey();

      InboundHandler handler;
      {
        std::lock_guard lock(mu);
        handler = inbound;
      }
      if (!handler) {
        (void)channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(false, "no handler")));
        {
          std::lock_guard lock(mu);
          ResetFieldsUnlocked();
        }
        return false;
      }

      const std::string peer_key = link.PeerKey();
      RunWorker(post_worker, [this, channel_session, params, handler = std::move(handler), peer_key]() mutable {
        CallMediaDirectConnectParams answer_params = params;
        CallMediaDirectCallbacks answer_cbs;
        handler(answer_params, answer_cbs);

        PostIo([this, channel_session, answer_params = std::move(answer_params),
                answer_cbs = std::move(answer_cbs), peer_key]() mutable {
          amp::PeerLink* resolved = runtime->Links().FindLink(peer_key);
          if (!resolved) {
            return;
          }
          CallMediaLegId leg_id{};
          {
            std::lock_guard lock(mu);
            if (phase.load(std::memory_order_acquire) != CallMediaSessionPhase::HelloInbound) {
              return;
            }
            if (offerer_glare && LocalWinsGlareForLink(*resolved)) {
              return;
            }
            if (answer_params.media_key.empty() || answer_params.call_id.empty()) {
              (void)channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(false, "rejected")));
              leg_id = active_leg_id;
            } else {
              if (!active_leg_id) {
                active_leg_id = CallMediaLegId{next_leg_id.fetch_add(1, std::memory_order_relaxed)};
                leg_finished_settled.store(false, std::memory_order_release);
              }
              leg_id = active_leg_id;
              active_params = answer_params;
              callbacks = std::move(answer_cbs);
              active_link = resolved;
              leg_phase.store(CallMediaLegPhase::AwaitingMedia, std::memory_order_release);
            }
          }
          if (answer_params.media_key.empty() || answer_params.call_id.empty()) {
            FailLeg(Error("amp call-media: inbound rejected"), /*remote_terminal=*/false);
            TeardownLeg(leg_id, /*finish_with_abort=*/false);
            return;
          }
          if (!channel_session->EnqueueOutbound(Utf8Body(BuildHelloAckJson(true)))) {
            FailLeg(Error("amp call-media: hello ack failed"), /*remote_terminal=*/false);
            TeardownLeg(leg_id, /*finish_with_abort=*/false);
            return;
          }
          control_ready.store(true, std::memory_order_release);
          TryEnterMediaReady();
        });
      });
      return true;
    }
    if (type == "hello_ack") {
      const bool ok = parsed->getIf<bool>("ok").value_or(false);
      if (!ok) {
        bool yield_glare = false;
        bool suppress_outbound_fail = false;
        {
          std::lock_guard lock(mu);
          const auto session_phase = phase.load(std::memory_order_acquire);
          if (session_phase == CallMediaSessionPhase::HelloInbound) {
            suppress_outbound_fail = true;
          } else if (session_phase == CallMediaSessionPhase::HelloOutbound && offerer_glare && active_link &&
                     !LocalWinsGlareForLink(*active_link)) {
            yield_glare = true;
          }
        }
        if (suppress_outbound_fail) {
          return true;
        }
        if (yield_glare) {
          AbandonOutbound();
          return true;
        }
        FailLeg(Error("amp call-media: hello rejected"), /*remote_terminal=*/false);
        TeardownLeg({}, /*finish_with_abort=*/false);
        return false;
      }
      control_ready.store(true, std::memory_order_release);
      leg_phase.store(CallMediaLegPhase::AwaitingMedia, std::memory_order_release);
      CallMediaLegId leg_id;
      {
        std::lock_guard lock(mu);
        leg_id = active_leg_id;
      }
      OpenMediaChannelOnActiveLink(leg_id, deadline);
      return true;
    }
    return false;
  }

  bool HandleMediaBody(const std::vector<uint8_t>& frame) {
    CallMediaDirectCallbacks cbs;
    CallMediaDirectConnectParams params;
    {
      std::lock_guard lock(mu);
      if (leg_phase.load(std::memory_order_acquire) != CallMediaLegPhase::MediaReady) {
        return true;
      }
      cbs = callbacks;
      params = active_params;
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

  void BindControlChannel(amp::PeerLink& link, const uint32_t channel_id, const Clock::time_point deadline,
                          const bool outbound) {
    {
      std::lock_guard lock(mu);
      if (phase.load(std::memory_order_acquire) == CallMediaSessionPhase::MediaReady) {
        return;
      }
      active_link = &link;
    }
    auto channel_session = std::make_shared<amp::ChannelSession>();
    channel_session->Bind(
        *link.Mux(), channel_id, amp::CallMediaControlChannelPolicy(),
        [this, channel_session, deadline, link_ptr = &link](Roe<std::vector<uint8_t>> frame) {
          if (!frame) {
            return false;
          }
          const std::string json_utf8(frame->begin(), frame->end());
          (void)HandleControlJson(json_utf8, channel_session, *link_ptr, deadline);
          return true;
        },
        [this](const char* reason) { OnChannelClosed(reason); });
    std::lock_guard lock(mu);
    control_session = channel_session;
    if (outbound) {
      outbound_control_session = std::move(channel_session);
    }
  }

  void BindMediaChannel(amp::PeerLink& link, const uint32_t channel_id) {
    {
      std::lock_guard lock(mu);
      active_link = &link;
    }
    auto channel_session = std::make_shared<amp::ChannelSession>();
    channel_session->Bind(
        *link.Mux(), channel_id, amp::CallMediaChannelPolicy(),
        [this](Roe<std::vector<uint8_t>> frame) {
          if (!frame) {
            return false;
          }
          return HandleMediaBody(*frame);
        },
        [this](const char* reason) { OnChannelClosed(reason); });
    {
      std::lock_guard lock(mu);
      media_session = std::move(channel_session);
    }
    media_bound.store(true, std::memory_order_release);
    TryEnterMediaReady();
  }

  void HandleInboundChannel(amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire)) {
      return;
    }
    if (!link.Mux()) {
      return;
    }
    const auto cls = link.Mux()->Class(channel_id);
    const auto deadline = connect_deadline.time_since_epoch().count() != 0
                              ? connect_deadline
                              : Clock::now() + std::chrono::seconds(15);
    if (cls == amp::ChannelClass::RealtimeControl) {
      BindControlChannel(link, channel_id, deadline, /*outbound=*/false);
      return;
    }
    if (cls == amp::ChannelClass::Realtime) {
      BindMediaChannel(link, channel_id);
    }
  }

  void BeginOutboundLeg(const CallMediaLegId leg_id, const CallMediaDirectConnectParams& params,
                        CallMediaDirectCallbacks callbacks, LegFinished on_finished, const int timeout_ms) {
    if (phase.load(std::memory_order_acquire) != CallMediaSessionPhase::Idle || active_leg_id.value != 0) {
      TeardownLeg({}, /*finish_with_abort=*/true);
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 15000);
    {
      std::lock_guard lock(mu);
      active_leg_id = leg_id;
      active_params = params;
      this->callbacks = std::move(callbacks);
      leg_finished = std::move(on_finished);
      leg_finished_settled.store(false, std::memory_order_release);
      local_cancel.store(false, std::memory_order_release);
      offerer_glare = params.offerer;
      connect_deadline = deadline;
      leg_phase.store(CallMediaLegPhase::ControlHello, std::memory_order_release);
      phase.store(CallMediaSessionPhase::HelloOutbound, std::memory_order_release);
    }

    auto& links = runtime->Links();
    const std::string peer_key = params.peer_key;
    const auto open_control = std::make_shared<std::function<void(int)>>();
    *open_control = [this, leg_id, peer_key, params, deadline, &links,
                     open_control](const int retries) {
      links.OpenChannel(peer_key, kCallMediaDirectProtocolId, amp::CallMediaControlChannelPolicy(),
                        [this, leg_id, peer_key, params, deadline, retries,
                         open_control](Roe<uint32_t> channel) mutable {
                          if (!channel) {
                            if (channel.error().message == "amp link: association not ready" && retries < 500 &&
                                active_leg_id.value == leg_id.value &&
                                !leg_finished_settled.load(std::memory_order_acquire)) {
                              PostIo([open_control, retries]() { (*open_control)(retries + 1); });
                              return;
                            }
                            FailLeg(channel.error(), /*remote_terminal=*/false);
                            TeardownLeg(leg_id, /*finish_with_abort=*/false);
                            return;
                          }
                          auto* link = runtime->Links().FindLink(peer_key);
                          if (!link) {
                            FailLeg(Error("amp call-media: peer link missing"), /*remote_terminal=*/false);
                            TeardownLeg(leg_id, /*finish_with_abort=*/false);
                            return;
                          }
                          {
                            std::lock_guard lock(mu);
                            if (active_leg_id.value != leg_id.value) {
                              return;
                            }
                            if (phase.load(std::memory_order_acquire) == CallMediaSessionPhase::HelloInbound &&
                                params.offerer && !LocalWinsGlareForLink(*link)) {
                              return;
                            }
                          }
                          ScheduleWhenChannelOpen(link, *channel, deadline,
                                                  [this, leg_id, link, channel = *channel, params,
                                                   deadline](const bool open) mutable {
                                                    if (!open || active_leg_id.value != leg_id.value) {
                                                      FailLeg(Error("amp call-media: channel open failed"),
                                                              /*remote_terminal=*/false);
                                                      TeardownLeg(leg_id, /*finish_with_abort=*/false);
                                                      return;
                                                    }
                                                    BindControlChannel(*link, channel, deadline, /*outbound=*/true);
                                                    std::shared_ptr<amp::ChannelSession> session;
                                                    {
                                                      std::lock_guard lock(mu);
                                                      session = control_session;
                                                    }
                                                    if (!session ||
                                                        !session->EnqueueOutbound(Utf8Body(BuildHelloJson(params)))) {
                                                      FailLeg(Error("amp call-media: hello write failed"),
                                                              /*remote_terminal=*/false);
                                                      TeardownLeg(leg_id, /*finish_with_abort=*/false);
                                                    }
                                                  });
                        });
    };
    (*open_control)(0);
  }
};

CallMediaLegCoordinator::CallMediaLegCoordinator(amp::MeshRuntime& runtime, WorkerPost post_worker)
    : impl_(std::make_unique<Impl>()), runtime_(runtime) {
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
  impl_->stopped.store(false, std::memory_order_release);
  runtime_.SetIoTick([impl = impl_.get()] { impl->TickConnectDeadline(); });
  runtime_.Links().SetProtocolHandler(kCallMediaDirectProtocolId,
                                      [impl = impl_.get()](amp::PeerLink& link, const uint32_t ch) {
                                        impl->HandleInboundChannel(link, ch);
                                      });
}

void CallMediaLegCoordinator::Stop() {
  impl_->started.store(false, std::memory_order_release);
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.SetIoTick({});
  runtime_.Links().RemoveProtocolHandler(kCallMediaDirectProtocolId);
  impl_->PostIo([impl = impl_.get()] { impl->TeardownLeg({}, /*finish_with_abort=*/true); });
  ClearInboundHandler();
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
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable { on_finished(Error("call-media service not started")); });
    }
    return {};
  }
  if (params.peer_key.empty() || params.call_id.empty() || params.media_key.empty()) {
    if (on_finished) {
      runtime_.PostToIo(
          [on_finished = std::move(on_finished)]() mutable { on_finished(Error("amp call-media: invalid connect params")); });
    }
    return {};
  }
  if (!runtime_.Links().GetLinkSnapshot(params.peer_key).has_endpoint) {
    if (on_finished) {
      runtime_.PostToIo(
          [on_finished = std::move(on_finished)]() mutable { on_finished(Error("amp call-media: peer endpoint not registered")); });
    }
    return {};
  }

  const CallMediaLegId leg_id{impl_->next_leg_id.fetch_add(1, std::memory_order_relaxed)};
  impl_->PostIo([impl = impl_.get(), leg_id, params, callbacks = std::move(callbacks),
                 on_finished = std::move(on_finished), timeout_ms]() mutable {
    impl->BeginOutboundLeg(leg_id, params, std::move(callbacks), std::move(on_finished), timeout_ms);
  });
  return leg_id;
}

void CallMediaLegCoordinator::CancelLeg(const CallMediaLegId id) {
  impl_->PostIo([impl = impl_.get(), id]() { impl->TeardownLeg(id, /*finish_with_abort=*/true); });
}

void CallMediaLegCoordinator::DetachLeg(const CallMediaLegId id) {
  impl_->PostIo([impl = impl_.get(), id]() { impl->TeardownLeg(id, /*finish_with_abort=*/true); });
  runtime_.Pump();
}

bool CallMediaLegCoordinator::IsLegActive(const CallMediaLegId id) const {
  if (!id) {
    return false;
  }
  std::lock_guard lock(impl_->mu);
  if (impl_->active_leg_id.value != id.value) {
    return false;
  }
  const auto session_phase = impl_->phase.load(std::memory_order_acquire);
  return session_phase != CallMediaSessionPhase::Idle && session_phase != CallMediaSessionPhase::Detaching;
}

CallMediaLegPhase CallMediaLegCoordinator::LegPhase(const CallMediaLegId id) const {
  if (!id) {
    return CallMediaLegPhase::Closed;
  }
  std::lock_guard lock(impl_->mu);
  if (impl_->active_leg_id.value != id.value) {
    return CallMediaLegPhase::Closed;
  }
  return impl_->leg_phase.load(std::memory_order_acquire);
}

CallMediaSessionPhase CallMediaLegCoordinator::Phase() const {
  return impl_->phase.load(std::memory_order_acquire);
}

Roe<void> CallMediaLegCoordinator::SendMedia(const CallMediaLegId id, const uint8_t channel,
                                             const std::vector<uint8_t>& payload, const uint32_t seq,
                                             const uint8_t mark) {
  std::shared_ptr<amp::ChannelSession> session;
  CallMediaDirectConnectParams params;
  {
    std::lock_guard lock(impl_->mu);
    if (impl_->active_leg_id.value != id.value ||
        impl_->leg_phase.load(std::memory_order_acquire) != CallMediaLegPhase::MediaReady || !impl_->media_session) {
      return Error("amp call-media: not in media ready");
    }
    session = impl_->media_session;
    params = impl_->active_params;
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
