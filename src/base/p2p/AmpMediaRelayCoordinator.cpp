#include "base/p2p/AmpMediaRelayCoordinator.h"

#include "base/mesh/channel/ChannelPolicy.h"
#include "base/mesh/link/PeerLink.h"
#include "base/p2p/MediaRelayAttachSm.h"
#include "base/p2p/MediaRelayLogic.h"
#include "common/ValueJson.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

std::string BodyToJson(const std::vector<uint8_t>& body) {
  return std::string(body.begin(), body.end());
}

std::string MakeSessionToken() {
  static std::atomic<uint64_t> seq{1};
  std::ostringstream oss;
  oss << "s" << seq.fetch_add(1, std::memory_order_relaxed);
  return oss.str();
}

MediaRelayQuote ParseQuoteResponse(const Object& root) {
  MediaRelayQuote q;
  q.ok = root.getIf<bool>("ok").value_or(false);
  q.error = root.getString("error").value_or("");
  q.quote_id = root.getString("quote_id").value_or("");
  q.a_up_bps = root.getIf<int64_t>("A_up").value_or(0);
  q.a_down_bps = root.getIf<int64_t>("A_down").value_or(0);
  q.b_up_bps = root.getIf<int64_t>("B_up").value_or(0);
  q.b_down_bps = root.getIf<int64_t>("B_down").value_or(0);
  q.pricing_mode = root.getString("mode").value_or("volunteer");
  q.rate = root.getIf<double>("rate").value_or(0.0);
  q.ceiling_bytes = root.getIf<int64_t>("ceiling_bytes").value_or(0);
  q.ceiling_amount = root.getIf<double>("ceiling_amount").value_or(0.0);
  return q;
}

} // namespace

struct AmpMediaRelayCoordinator::Impl {
  amp::MeshRuntime* runtime = nullptr;
  std::mutex mu;
  MediaRelayAdmissionPolicy admission;
  std::atomic<bool> started{false};
  std::atomic<bool> stopped{true};
  std::atomic<uint64_t> next_id{1};
  amp::MeshRuntime::IoTickId io_tick_id = 0;

  struct PendingQuote {
    MediaRelayQuote quote;
    std::string call_id;
  };

  struct HostCallSession {
    std::string call_id;
    std::string session_token;
    std::unordered_set<std::string> peers;
  };

  struct Session {
    MediaRelaySessionId id;
    MediaRelayBundleRole role = MediaRelayBundleRole::ClientQuote;
    MediaRelayBundlePhase phase = MediaRelayBundlePhase::Idle;
    Clock::time_point deadline{};
    std::string hop_peer_key;
    std::string call_id;
    std::string quote_id;
    std::string auth_stub;
    std::string session_token;
    std::shared_ptr<amp::ChannelSession> channel;
    MediaRelayAttachSm host_sm;
    QuoteFinished on_quote;
    AttachFinished on_attach;
    FrameHandler on_frame;
    bool finished = false;
    bool local_cancel = false;
  };

  std::unordered_map<uint64_t, std::unique_ptr<Session>> sessions;
  std::unordered_map<std::string, PendingQuote> quotes_by_id;
  std::unordered_map<std::string, std::shared_ptr<HostCallSession>> hosts_by_call;

  void PostIo(std::function<void()> task) {
    if (runtime && task) {
      runtime->PostToIo(std::move(task));
    }
  }

  Session* Find(const MediaRelaySessionId id) {
    auto it = sessions.find(id.value);
    return it == sessions.end() ? nullptr : it->second.get();
  }

  const Session* Find(const MediaRelaySessionId id) const {
    auto it = sessions.find(id.value);
    return it == sessions.end() ? nullptr : it->second.get();
  }

  void ScheduleWhenChannelOpen(amp::PeerLink* link, const uint32_t channel_id,
                               const Clock::time_point deadline, std::function<void(bool open)> done) {
    PostIo([this, link, channel_id, deadline, done = std::move(done)]() mutable {
      if (stopped.load(std::memory_order_acquire) || !link || !link->Mux()) {
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

  void TickDeadlines() {
    const auto now = Clock::now();
    std::vector<MediaRelaySessionId> timed_out;
    {
      std::lock_guard lock(mu);
      for (auto& [_, session] : sessions) {
        if (!session || session->finished || session->deadline.time_since_epoch().count() == 0) {
          continue;
        }
        if (now >= session->deadline && session->phase != MediaRelayBundlePhase::Attached &&
            session->phase != MediaRelayBundlePhase::HostServe) {
          timed_out.push_back(session->id);
        }
      }
    }
    for (const auto id : timed_out) {
      std::lock_guard lock(mu);
      if (auto* session = Find(id)) {
        TearDown(*session, false, false, "media-relay timed out");
      }
    }
  }

  void FinishQuote(Session& session, Roe<MediaRelayQuote> result) {
    if (session.finished) {
      return;
    }
    session.finished = true;
    auto cb = std::move(session.on_quote);
    session.on_quote = nullptr;
    if (cb) {
      cb(std::move(result));
    }
  }

  void FinishAttach(Session& session, Roe<MediaRelayAttachResult> result) {
    if (session.finished) {
      return;
    }
    session.finished = true;
    auto cb = std::move(session.on_attach);
    session.on_attach = nullptr;
    if (cb) {
      cb(std::move(result));
    }
  }

  void TearDown(Session& session, const bool suppress_notify, const bool local_cancel,
                const std::string& error) {
    session.local_cancel = local_cancel || session.local_cancel;
    session.phase = MediaRelayBundlePhase::Closing;
    if (session.channel && !session.channel->IsClosed()) {
      session.channel->CloseQuiet();
    }
    if (!session.finished) {
      if (session.role == MediaRelayBundleRole::ClientQuote) {
        FinishQuote(session, Error(error.empty() ? "media-relay aborted" : error));
      } else if (session.role == MediaRelayBundleRole::ClientAttach) {
        FinishAttach(session, Error(error.empty() ? "media-relay aborted" : error));
      } else {
        session.finished = true;
      }
    }
    (void)suppress_notify;
    sessions.erase(session.id.value);
  }

  void BindClientChannel(Session& session, amp::PeerLink& link, const uint32_t channel_id) {
    session.channel = std::make_shared<amp::ChannelSession>();
    const MediaRelaySessionId id = session.id;
    session.channel->Bind(
        *link.Mux(), channel_id, amp::MediaRelayClientChannelPolicy(),
        [this, id](Roe<std::vector<uint8_t>> frame) {
          std::lock_guard lock(mu);
          auto* session = Find(id);
          if (!session) {
            return false;
          }
          if (!frame) {
            TearDown(*session, false, false, "media-relay channel failed");
            return false;
          }
          auto root = TryParseObject(BodyToJson(*frame));
          if (!root) {
            TearDown(*session, false, false, "invalid media-relay json");
            return false;
          }
          if (session->role == MediaRelayBundleRole::ClientQuote &&
              session->phase == MediaRelayBundlePhase::WaitQuote) {
            const auto decision = DecideMediaRelayQuoteAck(
                {.phase = session->phase, .ack_ok = root->getIf<bool>("ok").value_or(false)});
            if (decision == MediaRelayQuoteAckDecision::IgnoreStale) {
              return true;
            }
            if (decision == MediaRelayQuoteAckDecision::Fail) {
              TearDown(*session, false, false, root->getString("error").value_or("quote failed"));
              return false;
            }
            auto quote = ParseQuoteResponse(*root);
            session->phase = MediaRelayBundlePhase::Closing;
            FinishQuote(*session, std::move(quote));
            if (session->channel) {
              session->channel->CloseQuiet();
            }
            sessions.erase(id.value);
            return false;
          }
          if (session->role == MediaRelayBundleRole::ClientAttach) {
            if (session->phase == MediaRelayBundlePhase::WaitAccept) {
              if (!root->getIf<bool>("ok").value_or(false)) {
                TearDown(*session, false, false, root->getString("error").value_or("accept failed"));
                return false;
              }
              session->session_token = root->getString("session_token").value_or("");
              session->phase = MediaRelayBundlePhase::OutboundAttach;
              Object attach_req;
              attach_req.set("v", int64_t{1});
              attach_req.set("op", "attach");
              attach_req.set("session_token", session->session_token);
              attach_req.set("call_id", session->call_id);
              attach_req.set("auth", session->auth_stub);
              if (!session->channel->EnqueueOutbound(JsonToBody(DumpJson(attach_req)))) {
                TearDown(*session, false, false, "failed to send attach");
                return false;
              }
              session->phase = MediaRelayBundlePhase::WaitAttachAck;
              return true;
            }
            if (session->phase == MediaRelayBundlePhase::WaitAttachAck) {
              const auto decision = DecideMediaRelayAttachAck(
                  {.phase = session->phase, .ack_ok = root->getIf<bool>("ok").value_or(false)});
              if (decision == MediaRelayAttachAckDecision::IgnoreStale) {
                return true;
              }
              if (decision == MediaRelayAttachAckDecision::Fail) {
                TearDown(*session, false, false, root->getString("error").value_or("attach failed"));
                return false;
              }
              session->phase = MediaRelayBundlePhase::Attached;
              MediaRelayAttachResult out;
              out.ok = true;
              out.session_token = session->session_token;
              FinishAttach(*session, std::move(out));
              return true;
            }
            if (session->phase == MediaRelayBundlePhase::Attached) {
              // Control JSON or media frames — D7b ignores DATA beyond handshake.
              return true;
            }
          }
          return true;
        },
        [this, id](const char*) {
          std::lock_guard lock(mu);
          if (auto* session = Find(id)) {
            const auto decision = DecideMediaRelayBundleClose({
                .phase = session->phase,
                .local_cancel = session->local_cancel,
                .remote_terminal = true,
                .finished = session->finished,
            });
            if (decision != MediaRelayBundleCloseDecision::Ignore) {
              TearDown(*session, decision == MediaRelayBundleCloseDecision::SuppressNotify,
                       session->local_cancel, "media-relay channel closed");
            }
          }
        });
  }

  void BeginQuote(Session& session, const MediaRelayQuoteRequest& request) {
    session.phase = MediaRelayBundlePhase::OutboundQuote;
    const MediaRelaySessionId id = session.id;
    const std::string hop = session.hop_peer_key;
    const auto deadline = session.deadline;
    Object req;
    req.set("v", int64_t{1});
    req.set("op", "quote");
    req.set("call_id", request.call_id);
    req.set("participants", int64_t{request.participants});
    req.set("want_up_bps", request.want_up_bps);
    req.set("want_down_bps", request.want_down_bps);
    const std::string json = DumpJson(req);

    runtime->Links().OpenChannel(
        hop, kMediaRelayProtocolId, amp::MediaRelayClientChannelPolicy(),
        [this, id, hop, deadline, json](Roe<uint32_t> channel) {
          amp::PeerLink* link = nullptr;
          uint32_t channel_id = 0;
          {
            std::lock_guard lock(mu);
            auto* session = Find(id);
            if (!session) {
              return;
            }
            if (!channel) {
              TearDown(*session, false, false, channel.error().message);
              return;
            }
            link = runtime->Links().FindLink(hop);
            if (!link) {
              TearDown(*session, false, false, "media-relay: hop link missing");
              return;
            }
            channel_id = *channel;
          }
          ScheduleWhenChannelOpen(link, channel_id, deadline, [this, id, hop, channel_id, json](const bool open) {
            std::lock_guard lock(mu);
            auto* session = Find(id);
            if (!session) {
              return;
            }
            if (!open) {
              TearDown(*session, false, false, "media-relay: channel open failed");
              return;
            }
            auto* link = runtime->Links().FindLink(hop);
            if (!link || !link->Mux()) {
              TearDown(*session, false, false, "media-relay: channel open failed");
              return;
            }
            BindClientChannel(*session, *link, channel_id);
            session->phase = MediaRelayBundlePhase::WaitQuote;
            if (!session->channel->EnqueueOutbound(JsonToBody(json))) {
              TearDown(*session, false, false, "failed to send quote");
            }
          });
        });
  }

  void BeginAttach(Session& session) {
    session.phase = MediaRelayBundlePhase::OutboundAccept;
    const MediaRelaySessionId id = session.id;
    const std::string hop = session.hop_peer_key;
    const auto deadline = session.deadline;
    Object accept_req;
    accept_req.set("v", int64_t{1});
    accept_req.set("op", "accept");
    accept_req.set("quote_id", session.quote_id);
    const std::string json = DumpJson(accept_req);

    runtime->Links().OpenChannel(
        hop, kMediaRelayProtocolId, amp::MediaRelayClientChannelPolicy(),
        [this, id, hop, deadline, json](Roe<uint32_t> channel) {
          amp::PeerLink* link = nullptr;
          uint32_t channel_id = 0;
          {
            std::lock_guard lock(mu);
            auto* session = Find(id);
            if (!session) {
              return;
            }
            if (!channel) {
              TearDown(*session, false, false, channel.error().message);
              return;
            }
            link = runtime->Links().FindLink(hop);
            if (!link) {
              TearDown(*session, false, false, "media-relay: hop link missing");
              return;
            }
            channel_id = *channel;
          }
          ScheduleWhenChannelOpen(link, channel_id, deadline, [this, id, hop, channel_id, json](const bool open) {
            std::lock_guard lock(mu);
            auto* session = Find(id);
            if (!session) {
              return;
            }
            if (!open) {
              TearDown(*session, false, false, "media-relay: channel open failed");
              return;
            }
            auto* link = runtime->Links().FindLink(hop);
            if (!link || !link->Mux()) {
              TearDown(*session, false, false, "media-relay: channel open failed");
              return;
            }
            BindClientChannel(*session, *link, channel_id);
            session->phase = MediaRelayBundlePhase::WaitAccept;
            if (!session->channel->EnqueueOutbound(JsonToBody(json))) {
              TearDown(*session, false, false, "failed to send accept");
            }
          });
        });
  }

  void RejectHost(amp::ChannelSession& channel, MediaRelayAttachSm& sm, const std::string& error,
                  const MediaRelayAttachEvent ev) {
    Object err;
    err.set("v", int64_t{1});
    err.set("ok", false);
    err.set("error", error);
    channel.EnqueueOutbound(JsonToBody(DumpJson(err)));
    (void)sm.Apply(ev);
    channel.Close();
  }

  void HandleInboundChannel(amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !link.Mux()) {
      return;
    }
    auto channel = std::make_shared<amp::ChannelSession>();
    auto host_sm = std::make_shared<MediaRelayAttachSm>();
    host_sm->remote = link.RemotePeerId();
    (void)host_sm->Apply(MediaRelayAttachEvent::StreamOpened);

    channel->Bind(*link.Mux(), channel_id, amp::MediaRelayClientChannelPolicy(),
                  [this, channel, host_sm](Roe<std::vector<uint8_t>> frame) {
                    if (!frame || stopped.load(std::memory_order_acquire)) {
                      return false;
                    }
                    auto root = TryParseObject(BodyToJson(*frame));
                    if (!root) {
                      RejectHost(*channel, *host_sm, "invalid media-relay json",
                                 MediaRelayAttachEvent::Cancel);
                      return false;
                    }
                    const std::string op = root->getString("op").value_or("");
                    std::lock_guard lock(mu);
                    MediaRelayOpAdmitContext admit;
                    admit.service_started = started.load(std::memory_order_acquire);
                    admit.stopping = stopped.load(std::memory_order_acquire);
                    admit.dialer_peer_id = host_sm->remote;
                    admit.op = op;
                    admit.call_id = root->getString("call_id").value_or(host_sm->call_id);
                    admit.session_exists_for_call =
                        !admit.call_id.empty() && hosts_by_call.contains(admit.call_id);
                    admit.serve_scope_mask = admission.serve_scope_mask;
                    admit.contact_peer_ids = admission.contact_peer_ids;

                    if (op == "quote") {
                      const auto decision = DecideMediaRelayOpAdmit(admit);
                      if (decision != MediaRelayOpAdmitDecision::Allow) {
                        RejectHost(*channel, *host_sm,
                                   decision == MediaRelayOpAdmitDecision::RefuseStranger
                                       ? "prefer contacts: stranger refused"
                                       : "media-relay not ready",
                                   MediaRelayAttachEvent::AdmitFail);
                        return false;
                      }
                      MediaRelayQuoteRequest req;
                      req.call_id = root->getString("call_id").value_or("");
                      req.participants = static_cast<int>(root->getNonNegInt("participants").value_or(1));
                      req.want_up_bps = root->getIf<int64_t>("want_up_bps").value_or(0);
                      req.want_down_bps = root->getIf<int64_t>("want_down_bps").value_or(0);
                      host_sm->call_id = req.call_id;
                      auto q = BuildDefaultMediaRelayQuote(req);
                      quotes_by_id[q.quote_id] = PendingQuote{q, req.call_id};
                      Object quote_resp;
                      quote_resp.set("v", int64_t{1});
                      quote_resp.set("ok", true);
                      quote_resp.set("op", "quote");
                      quote_resp.set("quote_id", q.quote_id);
                      quote_resp.set("A_up", q.a_up_bps);
                      quote_resp.set("A_down", q.a_down_bps);
                      quote_resp.set("B_up", q.b_up_bps);
                      quote_resp.set("B_down", q.b_down_bps);
                      quote_resp.set("mode", q.pricing_mode);
                      quote_resp.set("rate", q.rate);
                      quote_resp.set("ceiling_bytes", q.ceiling_bytes);
                      quote_resp.set("ceiling_amount", q.ceiling_amount);
                      channel->EnqueueOutbound(JsonToBody(DumpJson(quote_resp)));
                      (void)host_sm->Apply(MediaRelayAttachEvent::OpQuote);
                      return true;
                    }
                    if (op == "accept") {
                      if (host_sm->phase != MediaRelayAttachPhase::Control &&
                          host_sm->phase != MediaRelayAttachPhase::Quoted) {
                        RejectHost(*channel, *host_sm, "accept not allowed in phase",
                                   MediaRelayAttachEvent::OpAccept);
                        return false;
                      }
                      const std::string quote_id = root->getString("quote_id").value_or("");
                      auto it = quotes_by_id.find(quote_id);
                      if (it == quotes_by_id.end()) {
                        RejectHost(*channel, *host_sm, "unknown quote", MediaRelayAttachEvent::AttachFail);
                        return false;
                      }
                      admit.call_id = it->second.call_id;
                      admit.session_exists_for_call = hosts_by_call.contains(admit.call_id);
                      if (DecideMediaRelayOpAdmit(admit) != MediaRelayOpAdmitDecision::Allow) {
                        RejectHost(*channel, *host_sm, "prefer contacts: stranger refused",
                                   MediaRelayAttachEvent::AdmitFail);
                        return false;
                      }
                      host_sm->call_id = it->second.call_id;
                      host_sm->accepted_quote_id = quote_id;
                      host_sm->session_token = MakeSessionToken();
                      quotes_by_id.erase(it);
                      Object accept_resp;
                      accept_resp.set("v", int64_t{1});
                      accept_resp.set("ok", true);
                      accept_resp.set("op", "accept");
                      accept_resp.set("session_token", host_sm->session_token);
                      accept_resp.set("quote_id", host_sm->accepted_quote_id);
                      channel->EnqueueOutbound(JsonToBody(DumpJson(accept_resp)));
                      (void)host_sm->Apply(MediaRelayAttachEvent::OpAccept);
                      return true;
                    }
                    if (op == "attach") {
                      if (!host_sm->Apply(MediaRelayAttachEvent::OpAttach)) {
                        RejectHost(*channel, *host_sm, "attach not allowed in phase",
                                   MediaRelayAttachEvent::OpAttach);
                        return false;
                      }
                      const std::string token =
                          root->getString("session_token").value_or(host_sm->session_token);
                      const std::string call_id = root->getString("call_id").value_or("");
                      const std::string auth = root->getString("auth").value_or("");
                      host_sm->call_id = call_id;
                      if (token.empty() || call_id.empty()) {
                        RejectHost(*channel, *host_sm, "missing session_token or call_id",
                                   MediaRelayAttachEvent::AttachFail);
                        return false;
                      }
                      if (!MediaRelayAuthStubOk(auth, call_id)) {
                        RejectHost(*channel, *host_sm, "auth failed", MediaRelayAttachEvent::AttachFail);
                        return false;
                      }
                      admit.call_id = call_id;
                      admit.session_exists_for_call = hosts_by_call.contains(call_id);
                      if (DecideMediaRelayOpAdmit(admit) != MediaRelayOpAdmitDecision::Allow) {
                        RejectHost(*channel, *host_sm, "prefer contacts: stranger refused",
                                   MediaRelayAttachEvent::AdmitFail);
                        return false;
                      }
                      auto host = hosts_by_call[call_id];
                      if (!host) {
                        host = std::make_shared<HostCallSession>();
                        host->call_id = call_id;
                        host->session_token = token;
                        hosts_by_call[call_id] = host;
                      }
                      host->peers.insert(host_sm->remote);
                      Object attach_resp;
                      attach_resp.set("v", int64_t{1});
                      attach_resp.set("ok", true);
                      attach_resp.set("op", "attach");
                      channel->EnqueueOutbound(JsonToBody(DumpJson(attach_resp)));
                      (void)host_sm->Apply(MediaRelayAttachEvent::AttachOk);
                      return true; // keep open for future DATA (D7b+)
                    }
                    RejectHost(*channel, *host_sm, "unsupported op", MediaRelayAttachEvent::OpUnsupported);
                    return false;
                  });
  }
};

AmpMediaRelayCoordinator::AmpMediaRelayCoordinator(amp::MeshRuntime& runtime)
    : impl_(std::make_unique<Impl>()), runtime_(runtime) {
  impl_->runtime = &runtime_;
}

AmpMediaRelayCoordinator::~AmpMediaRelayCoordinator() {
  Stop();
}

void AmpMediaRelayCoordinator::Start() {
  if (impl_->started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  impl_->stopped.store(false, std::memory_order_release);
  impl_->io_tick_id = runtime_.AddIoTick([impl = impl_.get()] { impl->TickDeadlines(); });
  runtime_.Links().SetProtocolHandler(kMediaRelayProtocolId,
                                      [impl = impl_.get()](amp::PeerLink& link, const uint32_t ch) {
                                        impl->HandleInboundChannel(link, ch);
                                      });
}

void AmpMediaRelayCoordinator::Stop() {
  impl_->started.store(false, std::memory_order_release);
  impl_->stopped.store(true, std::memory_order_release);
  runtime_.RemoveIoTick(impl_->io_tick_id);
  impl_->io_tick_id = 0;
  runtime_.Links().RemoveProtocolHandler(kMediaRelayProtocolId);
  AbortInflight();
}

bool AmpMediaRelayCoordinator::IsStarted() const {
  return impl_->started.load(std::memory_order_acquire);
}

void AmpMediaRelayCoordinator::SetAdmissionPolicy(MediaRelayAdmissionPolicy policy) {
  std::lock_guard lock(impl_->mu);
  impl_->admission = std::move(policy);
}

void AmpMediaRelayCoordinator::AbortInflight() {
  impl_->PostIo([impl = impl_.get()] {
    std::lock_guard lock(impl->mu);
    std::vector<uint64_t> ids;
    for (auto& [id, _] : impl->sessions) {
      ids.push_back(id);
    }
    for (const auto id : ids) {
      if (auto* session = impl->Find(MediaRelaySessionId{id})) {
        impl->TearDown(*session, true, true, "media-relay aborted");
      }
    }
  });
}

MediaRelaySessionId AmpMediaRelayCoordinator::StartQuote(const std::string& hop_peer_key,
                                                         const MediaRelayQuoteRequest& request,
                                                         QuoteFinished on_finished, const int timeout_ms) {
  if (!impl_->started.load(std::memory_order_acquire)) {
    if (on_finished) {
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Error("media-relay service not started"));
      });
    }
    return {};
  }
  if (!runtime_.Links().GetLinkSnapshot(hop_peer_key).has_endpoint) {
    if (on_finished) {
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Error("hop peer endpoint not registered"));
      });
    }
    return {};
  }
  const MediaRelaySessionId id{impl_->next_id.fetch_add(1, std::memory_order_relaxed)};
  impl_->PostIo([impl = impl_.get(), id, hop_peer_key, request, on_finished = std::move(on_finished),
                 timeout_ms]() mutable {
    AmpMediaRelayCoordinator::Impl::Session* raw = nullptr;
    {
      std::lock_guard lock(impl->mu);
      auto session = std::make_unique<AmpMediaRelayCoordinator::Impl::Session>();
      session->id = id;
      session->role = MediaRelayBundleRole::ClientQuote;
      session->hop_peer_key = hop_peer_key;
      session->call_id = request.call_id;
      session->on_quote = std::move(on_finished);
      session->deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);
      raw = session.get();
      impl->sessions[id.value] = std::move(session);
    }
    impl->BeginQuote(*raw, request);
  });
  return id;
}

MediaRelaySessionId AmpMediaRelayCoordinator::StartAttach(
    const std::string& hop_peer_key, const std::string& quote_id, const std::string& call_id,
    const std::string& auth_stub, FrameHandler on_frame, AttachFinished on_finished, const int timeout_ms) {
  if (!impl_->started.load(std::memory_order_acquire)) {
    if (on_finished) {
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Error("media-relay service not started"));
      });
    }
    return {};
  }
  if (quote_id.empty() || call_id.empty()) {
    if (on_finished) {
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Error("missing quote_id or call_id"));
      });
    }
    return {};
  }
  if (!runtime_.Links().GetLinkSnapshot(hop_peer_key).has_endpoint) {
    if (on_finished) {
      runtime_.PostToIo([on_finished = std::move(on_finished)]() mutable {
        on_finished(Error("hop peer endpoint not registered"));
      });
    }
    return {};
  }
  const MediaRelaySessionId id{impl_->next_id.fetch_add(1, std::memory_order_relaxed)};
  impl_->PostIo([impl = impl_.get(), id, hop_peer_key, quote_id, call_id, auth_stub,
                 on_frame = std::move(on_frame), on_finished = std::move(on_finished),
                 timeout_ms]() mutable {
    AmpMediaRelayCoordinator::Impl::Session* raw = nullptr;
    {
      std::lock_guard lock(impl->mu);
      auto session = std::make_unique<AmpMediaRelayCoordinator::Impl::Session>();
      session->id = id;
      session->role = MediaRelayBundleRole::ClientAttach;
      session->hop_peer_key = hop_peer_key;
      session->quote_id = quote_id;
      session->call_id = call_id;
      session->auth_stub = auth_stub;
      session->on_frame = std::move(on_frame);
      session->on_attach = std::move(on_finished);
      session->deadline = Clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);
      raw = session.get();
      impl->sessions[id.value] = std::move(session);
    }
    impl->BeginAttach(*raw);
  });
  return id;
}

void AmpMediaRelayCoordinator::Cancel(const MediaRelaySessionId id) {
  if (!id) {
    return;
  }
  impl_->PostIo([impl = impl_.get(), id] {
    std::lock_guard lock(impl->mu);
    if (auto* session = impl->Find(id)) {
      impl->TearDown(*session, true, true, "media-relay aborted");
    }
  });
}

MediaRelayBundlePhase AmpMediaRelayCoordinator::Phase(const MediaRelaySessionId id) const {
  std::lock_guard lock(impl_->mu);
  if (const auto* session = impl_->Find(id)) {
    return session->phase;
  }
  return MediaRelayBundlePhase::Idle;
}

bool AmpMediaRelayCoordinator::IsSessionActive(const MediaRelaySessionId id) const {
  return MediaRelayBundlePhaseIsActive(Phase(id));
}

} // namespace pbr
