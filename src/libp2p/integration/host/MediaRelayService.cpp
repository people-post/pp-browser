#include "libp2p/integration/host/MediaRelayService.h"

#include "base/media/ByteRateLimiter.h"
#include "base/people/RelayScope.h"
#include "common/Logger.h"
#include "common/Utilities.h"
#include "libp2p/integration/host/Libp2pWorker.h"
#include "libp2p/integration/host/Libp2pExecutorLimits.h"
#include "libp2p/integration/host/MediaRelayAttachSm.h"
#include "libp2p/integration/host/MediaRelayFrames.h"
#include "libp2p/integration/host/MediaRelayLogic.h"
#include "libp2p/integration/host/StreamFrameIo.h"
#include "libp2p/integration/host/StreamJsonFrame.h"

#include <libp2p/basic/read.hpp>
#include <libp2p/basic/write.hpp>
#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace pbr {

namespace {

using libp2p::Bytes;
using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

auto MediaRelayLog() {
  return logging::getLogger("MediaRelayService");
}

/** Soft defaults when budget fields are 0 (unbounded / ops default). */
constexpr int64_t kDefaultUserUpBps = 500'000;
constexpr int64_t kDefaultUserDownBps = 2'000'000;
constexpr int64_t kDefaultSessionUpBps = 4'000'000;
constexpr int64_t kDefaultSessionDownBps = 16'000'000;
constexpr int64_t kDefaultCeilingBytes = 50'000'000;
/** path_pressure denominator: queued(1) + in-flight(1) on DuplexFrameSession. */
constexpr size_t kMaxOutboundBacklog = Libp2pExecutorLimits::kMaxMediaRelayOutboundFrames + 1;

LengthPrefixedFrameConfig MediaDataFrameConfig() {
  LengthPrefixedFrameConfig config;
  config.max_frame_bytes = kMaxMediaFrameBytes;
  config.allow_empty_body = true;
  return config;
}

Roe<std::vector<uint8_t>> ReadExactFrame(const std::shared_ptr<Stream>& stream) {
  return BlockingReadLengthPrefixedFrame(stream, MediaDataFrameConfig());
}

Roe<void> WriteExactBody(const std::shared_ptr<Stream>& stream, const std::vector<uint8_t>& body) {
  return BlockingWriteLengthPrefixedFrame(stream, body);
}

Roe<void> WriteJson(const std::shared_ptr<Stream>& stream, const nlohmann::json& root) {
  return BlockingWriteStreamJson(stream, root.dump());
}

Roe<nlohmann::json> ReadJson(const std::shared_ptr<Stream>& stream) {
  auto json_utf8 = BlockingReadStreamJson(stream);
  if (!json_utf8) {
    return json_utf8.error();
  }
  nlohmann::json root = nlohmann::json::parse(*json_utf8, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return Error("invalid media-relay json");
  }
  return root;
}

void RejectAndCloseAttach(MediaRelayAttachSm& sm, const std::shared_ptr<Stream>& s,
                          const std::string& error, const MediaRelayAttachEvent ev) {
  (void)WriteJson(s, {{"v", 1}, {"ok", false}, {"error", error}});
  s->close([](auto&&) {});
  // Always terminal — do not rely on Apply guards (wrong-phase reject uses OpAccept etc.).
  sm.SetPhase(MediaRelayAttachPhase::Rejected, ev);
  sm.SetPhase(MediaRelayAttachPhase::Closed, ev);
}

int64_t OrDefault(int64_t configured, int64_t fallback) {
  return configured > 0 ? configured : fallback;
}

std::string MakeId(const char* prefix) {
  static std::atomic<uint64_t> seq{1};
  return std::string(prefix) + std::to_string(seq.fetch_add(1));
}

uint64_t SubKey(uint32_t stream_id, uint16_t channel_id) {
  return (static_cast<uint64_t>(stream_id) << 16) | channel_id;
}

struct HostParticipant {
  std::string peer_id;
  std::shared_ptr<Stream> stream;
  /** Set for in-call hop local publisher (no network stream). */
  std::function<void(MediaDataFrame)> local_on_frame;
  std::shared_ptr<DuplexFrameSession> duplex;
  std::shared_ptr<std::atomic<bool>> duplex_cancelled;
  std::unordered_set<uint64_t> subscriptions;
  std::unordered_map<uint64_t, uint32_t> last_lossy_seq;
  int64_t a_up_bps = 0;
  int64_t a_down_bps = 0;
  int64_t bytes_up = 0;
  int64_t bytes_down = 0;
  ByteRateLimiter up_limiter;
  ByteRateLimiter down_limiter;
  uint64_t drops_rate = 0;
  uint64_t drops_queue = 0;
};

struct HostSession {
  std::string call_id;
  std::string session_token;
  int64_t b_up_bps = 0;
  int64_t b_down_bps = 0;
  int64_t ceiling_bytes = 0;
  int64_t bytes_total = 0;
  int64_t bytes_up_window = 0;
  int64_t bytes_down_window = 0;
  ByteRateLimiter session_up_limiter;
  ByteRateLimiter session_down_limiter;
  std::vector<std::shared_ptr<HostParticipant>> participants;
  uint64_t drops_total = 0;
  uint64_t drops_rate = 0;
  uint64_t drops_queue = 0;
  uint64_t drops_ceiling = 0;
};

struct PendingQuote {
  MediaRelayQuote quote;
  std::string call_id;
};

} // namespace

struct MediaRelayService::Impl : std::enable_shared_from_this<Impl> {
  std::mutex mu;
  Libp2pHost* host = nullptr;
  PeerSessionManager* sessions = nullptr;
  MediaRelayBudgetConfig budget;
  RelayPricingConfig pricing;
  MediaRelayAdmissionPolicy admission;

  std::unordered_map<std::string, PendingQuote> quotes_by_id;
  std::unordered_map<std::string, std::shared_ptr<HostSession>> sessions_by_token;
  std::unordered_map<std::string, std::shared_ptr<HostSession>> sessions_by_call;

  // Client attach state (remote hop dial) — N026 client phase machine
  std::atomic<MediaRelayClientPhase> client_phase{MediaRelayClientPhase::Idle};
  std::string client_call_id;
  std::shared_ptr<Stream> client_stream;
  std::shared_ptr<DuplexFrameSession> client_duplex;
  std::shared_ptr<std::atomic<bool>> client_duplex_cancelled;
  std::string client_session_token;
  std::function<void(MediaDataFrame)> client_on_frame;
  /** Local mirror of subscribe ops already sent on client_stream (dedupe wire spam). */
  std::unordered_set<uint64_t> client_subscriptions;
  /** Bumped on Detach — stale duplex handlers exit. */
  std::atomic<uint64_t> client_reader_epoch{0};
  /**
   * Fired on unexpected client duplex death (read EOF / framing / handler), not intentional
   * Detach/Stop. Topology re-AcceptAndAttach while the call is still SFU-live.
   */
  std::function<void()> client_transport_lost_handler;
  /** AcceptAndAttach waiter — Detach / timeout complete it (call-media pattern). */
  std::shared_ptr<std::atomic<bool>> client_attach_settled;
  std::shared_ptr<std::promise<Roe<MediaRelayAttachResult>>> client_attach_promise;

  MediaRelayClientPhase ClientPhase() const {
    return client_phase.load(std::memory_order_acquire);
  }

  void SetClientPhaseLocked(MediaRelayClientPhase next, MediaRelayClientEvent ev,
                            const std::string& call_id = {}) {
    const MediaRelayClientPhase prev = client_phase.load(std::memory_order_relaxed);
    if (!call_id.empty()) {
      client_call_id = call_id;
    }
    if (next == MediaRelayClientPhase::Idle) {
      client_call_id.clear();
    }
    if (prev == next) {
      MediaRelayLog().info << "media_relay_client phase=" << MediaRelayClientPhaseName(prev)
                           << " event=" << MediaRelayClientEventName(ev)
                           << " call_id=" << client_call_id;
      return;
    }
    client_phase.store(next, std::memory_order_release);
    MediaRelayLog().info << "media_relay_client phase=" << MediaRelayClientPhaseName(prev) << "->"
                         << MediaRelayClientPhaseName(next)
                         << " event=" << MediaRelayClientEventName(ev)
                         << " call_id=" << client_call_id;
  }

  void IgnoreClientEventLocked(MediaRelayClientEvent ev, const char* reason) {
    MediaRelayLog().warning << "media_relay_client ignore event=" << MediaRelayClientEventName(ev)
                            << " phase=" << MediaRelayClientPhaseName(ClientPhase()) << " reason="
                            << (reason ? reason : "");
  }

  /**
   * Sole legal client-phase transition entry (CallLifecycle-style). Must hold mu.
   * Returns false when the event is ignored for the current phase.
   */
  bool ApplyClientLocked(MediaRelayClientEvent ev, const std::string& call_id = {}) {
    const MediaRelayClientPhase p = ClientPhase();
    const MediaRelayClientPhaseOutcome outcome = DecideMediaRelayClientPhase(p, ev);
    switch (outcome.decision) {
    case MediaRelayClientPhaseDecision::Transition:
      SetClientPhaseLocked(outcome.next, ev, call_id);
      return true;
    case MediaRelayClientPhaseDecision::Keep:
      return true;
    case MediaRelayClientPhaseDecision::Ignore:
      if (ev == MediaRelayClientEvent::OpenStreamOk) {
        IgnoreClientEventLocked(ev, "not dialing");
      } else if (ev == MediaRelayClientEvent::AcceptOk) {
        IgnoreClientEventLocked(ev, "not accepting");
      } else if (ev == MediaRelayClientEvent::AttachOk) {
        IgnoreClientEventLocked(ev, "not attaching");
      } else if (ev == MediaRelayClientEvent::DuplexLost) {
        IgnoreClientEventLocked(ev, "already detaching or idle");
      } else if (ev == MediaRelayClientEvent::OpenStreamFail ||
                 ev == MediaRelayClientEvent::AcceptFail ||
                 ev == MediaRelayClientEvent::AttachFail ||
                 ev == MediaRelayClientEvent::AttachTimeout ||
                 ev == MediaRelayClientEvent::AttachSuperseded) {
        IgnoreClientEventLocked(ev, "not in attach flight");
      } else {
        IgnoreClientEventLocked(ev, "unhandled event");
      }
      return false;
    }
    IgnoreClientEventLocked(ev, "unhandled event");
    return false;
  }

  void CompleteClientAttachLocked(Roe<MediaRelayAttachResult> value) {
    if (!client_attach_settled) {
      return;
    }
    if (!client_attach_settled->exchange(true)) {
      try {
        if (client_attach_promise) {
          client_attach_promise->set_value(std::move(value));
        }
      } catch (const std::future_error&) {
      }
    }
    client_attach_settled.reset();
    client_attach_promise.reset();
  }

  bool ClientAttachWaiterActiveLocked() const {
    return client_attach_settled && !client_attach_settled->load(std::memory_order_acquire);
  }

  // In-call hop: local publisher joined into HostSession without dialing self
  std::shared_ptr<HostParticipant> local_hop_part;
  std::shared_ptr<HostSession> local_hop_session;
  std::string local_hop_peer_id;

  bool AdmitPeer(const std::string& peer_id) {
    return RelayAdmissionAllowsDialer(admission.serve_scope_mask, peer_id, admission.contact_peer_ids);
  }

  /**
   * Call-scoped admission: first dialer for call_id must pass contact/scope admission.
   * Once a HostSession exists for call_id (sponsor or local hop attached), further dialers
   * for that call are admitted even if strangers to this hop.
   */
  bool AdmitPeerForCall(const std::string& peer_id, const std::string& call_id) {
    const bool session_exists =
        !call_id.empty() && sessions_by_call.find(call_id) != sessions_by_call.end();
    return MediaRelayCallScopedAdmit(session_exists, AdmitPeer(peer_id));
  }

  MediaRelayQuote BuildQuote(const MediaRelayQuoteRequest& req) {
    MediaRelayQuote q;
    q.ok = true;
    q.quote_id = MakeId("q");
    q.a_up_bps = OrDefault(budget.default_per_user_up_bps, kDefaultUserUpBps);
    q.a_down_bps = OrDefault(budget.default_per_user_down_bps, kDefaultUserDownBps);
    q.b_up_bps = OrDefault(budget.max_session_up_bps, kDefaultSessionUpBps);
    q.b_down_bps = OrDefault(budget.max_session_down_bps, kDefaultSessionDownBps);
    if (req.want_up_bps > 0) {
      q.a_up_bps = std::min(q.a_up_bps, req.want_up_bps);
    }
    if (req.want_down_bps > 0) {
      q.a_down_bps = std::min(q.a_down_bps, req.want_down_bps);
    }
    q.pricing_mode = pricing.mode.empty() ? "volunteer" : pricing.mode;
    q.rate = (q.pricing_mode == "volunteer") ? 0.0 : pricing.rate;
    q.ceiling_bytes = kDefaultCeilingBytes;
    q.ceiling_amount = 0.0;
    return q;
  }

  /** Post a client→hop frame on the duplex (io_context). */
  bool EnqueueClientBody(std::vector<uint8_t> body) {
    if (!host) {
      return false;
    }
    std::shared_ptr<DuplexFrameSession> duplex;
    {
      std::lock_guard<std::mutex> lock(mu);
      duplex = client_duplex;
    }
    if (!duplex) {
      return false;
    }
    host->Post([duplex = std::move(duplex), body = std::move(body)]() mutable {
      (void)duplex->EnqueueOutbound(std::move(body));
    });
    return true;
  }

  void StopClientDuplexLocked() {
    if (client_duplex_cancelled) {
      client_duplex_cancelled->store(true, std::memory_order_release);
    }
    std::shared_ptr<DuplexFrameSession> duplex = std::move(client_duplex);
    client_duplex_cancelled.reset();
    if (duplex) {
      duplex->Stop();
      if (host) {
        host->Post([duplex = std::move(duplex)]() mutable { duplex.reset(); });
      }
    }
  }

  void StartClientDuplex(const std::shared_ptr<Impl>& self) {
    if (!host) {
      return;
    }
    std::shared_ptr<Stream> stream;
    std::shared_ptr<DuplexFrameSession> duplex;
    std::shared_ptr<std::atomic<bool>> cancelled;
    uint64_t epoch = 0;
    {
      std::lock_guard<std::mutex> lock(mu);
      stream = client_stream;
      if (!stream) {
        return;
      }
      StopClientDuplexLocked();
      epoch = client_reader_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
      duplex = std::make_shared<DuplexFrameSession>();
      cancelled = std::make_shared<std::atomic<bool>>(false);
      client_duplex = duplex;
      client_duplex_cancelled = cancelled;
    }
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    host->Post([self, duplex, stream, cancelled, epoch, done]() {
      if (!duplex || !stream) {
        done->set_value();
        return;
      }
      duplex->Start(
          stream,
          [self, epoch](Roe<std::vector<uint8_t>> frame_res) {
            if (self->client_reader_epoch.load(std::memory_order_acquire) != epoch) {
              return false;
            }
            if (!frame_res) {
              return false;
            }
            if (frame_res->empty() || (*frame_res)[0] == '{') {
              return true; // keep reading past empty / hop JSON acks
            }
            auto frame = DecodeMediaDataFrame(*frame_res);
            if (!frame) {
              // Skip corrupt frame; do not tear down the duplex (old BlockingRead exited).
              return true;
            }
            std::function<void(MediaDataFrame)> cb;
            {
              std::lock_guard<std::mutex> lock(self->mu);
              cb = self->client_on_frame;
            }
            if (cb) {
              cb(*frame);
            }
            return true;
          },
          [cancelled]() { return cancelled && cancelled->load(std::memory_order_acquire); },
          MediaDataFrameConfig(),
          [self, epoch, cancelled](const char* reason) {
            // Intentional Detach/Stop sets cancelled before Stop (on_closed cleared) or bumps epoch.
            if (cancelled && cancelled->load(std::memory_order_acquire)) {
              return;
            }
            if (self->client_reader_epoch.load(std::memory_order_acquire) != epoch) {
              return;
            }
            std::function<void()> handler;
            {
              std::lock_guard<std::mutex> lock(self->mu);
              if (self->client_reader_epoch.load(std::memory_order_relaxed) != epoch) {
                return;
              }
              // Drop dead transport so IsAttached/SendFrame reflect reality; keep on_frame for reattach.
              // ApplyClientLocked ignores DuplexLost when already Idle/Detaching (intentional Detach).
              if (!self->ApplyClientLocked(MediaRelayClientEvent::DuplexLost, self->client_call_id)) {
                return;
              }
              self->client_duplex.reset();
              self->client_duplex_cancelled.reset();
              self->client_stream.reset();
              self->client_session_token.clear();
              self->client_subscriptions.clear();
              handler = self->client_transport_lost_handler;
            }
            MediaRelayLog().warning << "client duplex lost reason="
                                    << (reason && reason[0] ? reason : "unknown")
                                    << " will_notify=" << (handler ? 1 : 0);
            if (handler) {
              handler();
            }
          },
          Libp2pExecutorLimits::kMaxMediaRelayClientOutboundFrames,
          []() {},
          /*write_preferred=*/true);
      done->set_value();
    });
    fut.wait();
  }

  /** Enqueue a fanout body on the peer's duplex (host io_context). */
  void EnqueueFanoutBody(const std::shared_ptr<HostParticipant>& part, std::vector<uint8_t> body) {
    if (!host || !part) {
      return;
    }
    auto duplex = part->duplex;
    if (!duplex) {
      return;
    }
    host->Post([duplex = std::move(duplex), body = std::move(body)]() mutable {
      (void)duplex->EnqueueOutbound(std::move(body));
    });
  }

  void Fanout(const std::shared_ptr<HostSession>& session, const std::string& from_peer,
              const MediaDataFrame& frame, const std::vector<uint8_t>& body) {
    if (!host) {
      return;
    }
    const uint64_t key = SubKey(frame.stream_id, frame.channel_id);
    const int64_t now_ms = util::NowUnixMs();
    const int64_t nbytes = static_cast<int64_t>(body.size());
    std::vector<std::shared_ptr<HostParticipant>> parts;
    {
      std::lock_guard<std::mutex> lock(mu);
      parts = session->participants;
    }
    for (const auto& part : parts) {
      if (!part || part->peer_id == from_peer) {
        continue;
      }
      bool local = false;
      bool over_ceiling = false;
      bool rate_limited = false;
      std::function<void(MediaDataFrame)> on_frame;
      std::shared_ptr<DuplexFrameSession> duplex;
      {
        std::lock_guard<std::mutex> lock(mu);
        local = static_cast<bool>(part->local_on_frame);
        if (!local && !part->duplex) {
          continue;
        }
        if (part->subscriptions.find(key) == part->subscriptions.end()) {
          continue;
        }
        if (frame.channel_type == MediaChannelType::LatestLossy) {
          auto it = part->last_lossy_seq.find(key);
          if (ShouldDropStaleLossyFrame(it != part->last_lossy_seq.end(),
                                        it != part->last_lossy_seq.end() ? it->second : 0, frame.seq,
                                        frame.mark)) {
            continue;
          }
          part->last_lossy_seq[key] = frame.seq;
        }
        over_ceiling = session->ceiling_bytes > 0 && session->bytes_total > session->ceiling_bytes;
        if (over_ceiling) {
          ++session->drops_total;
          ++session->drops_ceiling;
          continue;
        }
        if (!part->down_limiter.TryConsume(nbytes, now_ms) ||
            !session->session_down_limiter.TryConsume(nbytes, now_ms)) {
          ++part->drops_rate;
          ++session->drops_total;
          ++session->drops_rate;
          rate_limited = true;
          continue;
        }
        part->bytes_down += nbytes;
        session->bytes_total += nbytes;
        session->bytes_down_window += nbytes;
        if (local) {
          on_frame = part->local_on_frame;
        } else {
          duplex = part->duplex;
        }
      }
      if (over_ceiling || rate_limited) {
        continue;
      }
      if (local) {
        if (on_frame) {
          on_frame(frame);
        }
        continue;
      }
      if (duplex) {
        EnqueueFanoutBody(part, body);
      }
    }
  }

  /** Returns false when the participant session should end (explicit detach only). */
  bool ProcessParticipantFrame(const std::shared_ptr<HostSession>& session,
                               const std::shared_ptr<HostParticipant>& part,
                               const std::vector<uint8_t>& body) {
    if (body.empty()) {
      return true; // skip; do not tear down
    }
    if (body[0] == '{') {
      nlohmann::json root =
          nlohmann::json::parse(std::string(body.begin(), body.end()), nullptr, false);
      if (root.is_discarded() || !root.is_object()) {
        return true; // skip corrupt control; keep uplink
      }
      const std::string op = root.value("op", "");
      if (op == "subscribe") {
        {
          std::lock_guard<std::mutex> lock(mu);
          part->subscriptions.insert(SubKey(root.value("stream_id", 0u),
                                            static_cast<uint16_t>(root.value("channel_id", 0))));
        }
        logging::getLogger("MediaRelayService").info
            << "hop subscribe peer=" << part->peer_id << " stream=" << root.value("stream_id", 0u)
            << " ch=" << root.value("channel_id", 0) << " call=" << session->call_id
            << " parts=" << session->participants.size();
        if (part->duplex) {
          const std::string json =
              nlohmann::json({{"v", 1}, {"ok", true}, {"op", "subscribe"}}).dump();
          part->duplex->EnqueueOutbound(std::vector<uint8_t>(json.begin(), json.end()));
        }
      } else if (op == "unsubscribe") {
        {
          std::lock_guard<std::mutex> lock(mu);
          part->subscriptions.erase(SubKey(root.value("stream_id", 0u),
                                             static_cast<uint16_t>(root.value("channel_id", 0))));
        }
        if (part->duplex) {
          const std::string json =
              nlohmann::json({{"v", 1}, {"ok", true}, {"op", "unsubscribe"}}).dump();
          part->duplex->EnqueueOutbound(std::vector<uint8_t>(json.begin(), json.end()));
        }
      } else if (op == "detach") {
        if (part->duplex) {
          const std::string json = nlohmann::json({{"v", 1}, {"ok", true}, {"op", "detach"}}).dump();
          part->duplex->EnqueueOutbound(std::vector<uint8_t>(json.begin(), json.end()));
        }
        return false;
      }
      return true;
    }

    auto frame = DecodeMediaDataFrame(body);
    if (!frame) {
      // Skip corrupt media; do not remove the participant (matches client duplex policy).
      return true;
    }
    const int64_t nbytes = static_cast<int64_t>(body.size());
    const int64_t now_ms = util::NowUnixMs();
    {
      std::lock_guard<std::mutex> lock(mu);
      if (!part->up_limiter.TryConsume(nbytes, now_ms) ||
          !session->session_up_limiter.TryConsume(nbytes, now_ms)) {
        ++part->drops_rate;
        ++session->drops_total;
        ++session->drops_rate;
        return true; // drop excess uplink; keep session
      }
      if (session->ceiling_bytes > 0 && session->bytes_total > session->ceiling_bytes) {
        ++session->drops_total;
        ++session->drops_ceiling;
        return true;
      }
      part->bytes_up += nbytes;
      session->bytes_total += nbytes;
      session->bytes_up_window += nbytes;
    }
    Fanout(session, part->peer_id, *frame, body);
    return true;
  }

  static void ConfigureParticipantLimiters(HostParticipant& part) {
    part.up_limiter.Configure(part.a_up_bps);
    part.down_limiter.Configure(part.a_down_bps);
  }

  static void ConfigureSessionLimiters(HostSession& session) {
    session.session_up_limiter.Configure(session.b_up_bps);
    session.session_down_limiter.Configure(session.b_down_bps);
  }

  bool CanOpenNewHostSessionLocked() const {
    return MediaRelayCanOpenHostSession(sessions_by_call.size(), MediaRelayService::kMaxHostSessions);
  }

  static bool CanAddParticipantLocked(const HostSession& session) {
    return MediaRelayCanAddParticipant(session.participants.size(),
                                       MediaRelayService::kMaxParticipantsPerSession);
  }

  void CleanupParticipant(const std::shared_ptr<HostSession>& session,
                          const std::shared_ptr<HostParticipant>& part, const char* reason) {
    logging::getLogger("MediaRelayService").warning
        << "CleanupParticipant peer=" << (part ? part->peer_id : "(null)")
        << " call=" << (session ? session->call_id : "")
        << " reason=" << (reason && reason[0] ? reason : "unknown")
        << " up=" << (part ? part->bytes_up : 0) << " dn=" << (part ? part->bytes_down : 0)
        << " parts=" << (session ? session->participants.size() : 0);
    if (part->duplex_cancelled) {
      part->duplex_cancelled->store(true, std::memory_order_release);
    }
    // Move duplex out before Stop — CloseSession may be on the call stack (FIN path).
    std::shared_ptr<DuplexFrameSession> duplex = std::move(part->duplex);
    if (duplex) {
      duplex->Stop();
    }
    std::lock_guard<std::mutex> lock(mu);
    session->participants.erase(std::remove_if(session->participants.begin(), session->participants.end(),
                                               [&](const std::shared_ptr<HostParticipant>& p) {
                                                 return p.get() == part.get();
                                               }),
                                session->participants.end());
    if (part->stream) {
      part->stream->close([](auto&&) {});
    }
    // Defer destroy so we are not inside DuplexFrameSession::CloseSession.
    if (duplex && host) {
      host->Post([duplex = std::move(duplex)]() mutable { duplex.reset(); });
    }
  }

  void InitParticipantAsyncSync(const std::shared_ptr<HostSession>& session,
                                const std::shared_ptr<HostParticipant>& part) {
    if (!host || !part->stream) {
      return;
    }
    part->duplex = std::make_shared<DuplexFrameSession>();
    part->duplex_cancelled = std::make_shared<std::atomic<bool>>(false);
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    auto self = shared_from_this();
    host->Post([self, session, part, done]() {
      if (!part->duplex || !part->stream) {
        done->set_value();
        return;
      }
      const auto cancel_check = [cancelled = part->duplex_cancelled]() {
        return cancelled && cancelled->load(std::memory_order_acquire);
      };
      part->duplex->Start(
          part->stream,
          [self, session, part](Roe<std::vector<uint8_t>> frame_res) {
            if (!frame_res) {
              return false;
            }
            return self->ProcessParticipantFrame(session, part, *frame_res);
          },
          cancel_check, MediaDataFrameConfig(),
          [self, session, part](const char* reason) {
            self->CleanupParticipant(session, part, reason);
          },
          Libp2pExecutorLimits::kMaxMediaRelayOutboundFrames,
          [self, session, part]() {
            std::lock_guard<std::mutex> lock(self->mu);
            ++part->drops_queue;
            ++session->drops_total;
            ++session->drops_queue;
          },
          /*write_preferred=*/true);
      done->set_value();
    });
    fut.wait();
  }

  void StartParticipantAsync(const std::shared_ptr<HostSession>& session,
                             const std::shared_ptr<HostParticipant>& part) {
    InitParticipantAsyncSync(session, part);
  }

  void HandleInbound(libp2p::StreamAndProtocol stream_and_protocol) {
    if (!host) {
      return;
    }
    auto stream = std::move(stream_and_protocol.stream);
    auto self = shared_from_this();
    PostLibp2pWorker(*host, WorkerLane::Normal, [self, stream = std::move(stream)]() mutable {
      self->HandleInboundBody(std::move(stream));
    });
  }

  void HandleInboundBody(std::shared_ptr<Stream> stream) {
    // Per-inbound-stream attach SM (N026). HostSession remains a map object; this only
    // sequences quote → accept → attach on one control stream.
    MediaRelayAttachSm sm;
    if (auto peer = stream->remotePeerId()) {
      sm.remote = peer.value().toBase58();
    }
    (void)sm.Apply(MediaRelayAttachEvent::StreamOpened);

    // Admission runs per control op (quote/accept/attach) with call_id so joiners can
    // attach on a fresh stream after an admitted sponsor opened the session.
    std::shared_ptr<HostSession> session;

    while (sm.phase != MediaRelayAttachPhase::Attached && sm.phase != MediaRelayAttachPhase::Rejected &&
           sm.phase != MediaRelayAttachPhase::Closed) {
      auto root = ReadJson(stream);
      if (!root) {
        (void)sm.Apply(MediaRelayAttachEvent::Cancel);
        stream->close([](auto&&) {});
        return;
      }
      const std::string op = root->value("op", "");
      if (op == "quote") {
        MediaRelayQuoteRequest req;
        req.call_id = root->value("call_id", "");
        req.participants = root->value("participants", 1);
        req.want_up_bps = root->value("want_up_bps", static_cast<int64_t>(0));
        req.want_down_bps = root->value("want_down_bps", static_cast<int64_t>(0));
        sm.call_id = req.call_id;
        {
          std::lock_guard<std::mutex> lock(mu);
          if (!AdmitPeerForCall(sm.remote, req.call_id)) {
            RejectAndCloseAttach(sm, stream, "prefer contacts: stranger refused",
                                 MediaRelayAttachEvent::AdmitFail);
            return;
          }
        }
        MediaRelayQuote q = BuildQuote(req);
        {
          std::lock_guard<std::mutex> lock(mu);
          quotes_by_id[q.quote_id] = PendingQuote{q, req.call_id};
        }
        (void)WriteJson(stream, {{"v", 1},
                                 {"ok", true},
                                 {"op", "quote"},
                                 {"quote_id", q.quote_id},
                                 {"A_up", q.a_up_bps},
                                 {"A_down", q.a_down_bps},
                                 {"B_up", q.b_up_bps},
                                 {"B_down", q.b_down_bps},
                                 {"mode", q.pricing_mode},
                                 {"rate", q.rate},
                                 {"ceiling_bytes", q.ceiling_bytes},
                                 {"ceiling_amount", q.ceiling_amount}});
        (void)sm.Apply(MediaRelayAttachEvent::OpQuote);
      } else if (op == "accept") {
        // Accept from Control or Quoted (quote may have been issued on another stream).
        if (sm.phase != MediaRelayAttachPhase::Control && sm.phase != MediaRelayAttachPhase::Quoted) {
          RejectAndCloseAttach(sm, stream, "accept not allowed in phase", MediaRelayAttachEvent::OpAccept);
          return;
        }
        const std::string quote_id = root->value("quote_id", "");
        PendingQuote pending;
        {
          std::lock_guard<std::mutex> lock(mu);
          auto it = quotes_by_id.find(quote_id);
          if (it == quotes_by_id.end()) {
            RejectAndCloseAttach(sm, stream, "unknown quote", MediaRelayAttachEvent::AttachFail);
            return;
          }
          pending = it->second;
          if (!AdmitPeerForCall(sm.remote, pending.call_id)) {
            RejectAndCloseAttach(sm, stream, "prefer contacts: stranger refused",
                                 MediaRelayAttachEvent::AdmitFail);
            return;
          }
          quotes_by_id.erase(it);
        }
        sm.call_id = pending.call_id;
        sm.accepted_quote_id = quote_id;
        sm.session_token = MakeId("s");
        (void)WriteJson(stream, {{"v", 1},
                                 {"ok", true},
                                 {"op", "accept"},
                                 {"session_token", sm.session_token},
                                 {"quote_id", sm.accepted_quote_id}});
        (void)sm.Apply(MediaRelayAttachEvent::OpAccept);
      } else if (op == "attach") {
        if (!sm.Apply(MediaRelayAttachEvent::OpAttach)) {
          RejectAndCloseAttach(sm, stream, "attach not allowed in phase", MediaRelayAttachEvent::OpAttach);
          return;
        }
        const std::string token = root->value("session_token", sm.session_token);
        const std::string call_id = root->value("call_id", "");
        const std::string auth = root->value("auth", "");
        sm.call_id = call_id;
        if (token.empty() || call_id.empty()) {
          RejectAndCloseAttach(sm, stream, "missing session_token or call_id",
                               MediaRelayAttachEvent::AttachFail);
          return;
        }
        // Auth stub: non-empty auth required; must equal call_id for v1 dogfood.
        if (!MediaRelayAuthStubOk(auth, call_id)) {
          RejectAndCloseAttach(sm, stream, "auth failed", MediaRelayAttachEvent::AttachFail);
          return;
        }
        {
          std::lock_guard<std::mutex> lock(mu);
          if (!AdmitPeerForCall(sm.remote, call_id)) {
            RejectAndCloseAttach(sm, stream, "prefer contacts: stranger refused",
                                 MediaRelayAttachEvent::AdmitFail);
            return;
          }
        }

        auto part = std::make_shared<HostParticipant>();
        part->peer_id = sm.remote;
        part->stream = stream;

        {
          std::lock_guard<std::mutex> lock(mu);
          auto it = sessions_by_call.find(call_id);
          if (it != sessions_by_call.end()) {
            session = it->second;
            if (!CanAddParticipantLocked(*session)) {
              RejectAndCloseAttach(sm, stream, "session participant limit",
                                   MediaRelayAttachEvent::AttachFail);
              return;
            }
          } else {
            if (!CanOpenNewHostSessionLocked()) {
              RejectAndCloseAttach(sm, stream, "host session limit", MediaRelayAttachEvent::AttachFail);
              return;
            }
            session = std::make_shared<HostSession>();
            session->call_id = call_id;
            session->session_token = token;
            session->b_up_bps = OrDefault(budget.max_session_up_bps, kDefaultSessionUpBps);
            session->b_down_bps = OrDefault(budget.max_session_down_bps, kDefaultSessionDownBps);
            session->ceiling_bytes = kDefaultCeilingBytes;
            ConfigureSessionLimiters(*session);
            sessions_by_call[call_id] = session;
            sessions_by_token[token] = session;
          }
          part->a_up_bps = OrDefault(budget.default_per_user_up_bps, kDefaultUserUpBps);
          part->a_down_bps = OrDefault(budget.default_per_user_down_bps, kDefaultUserDownBps);
          ConfigureParticipantLimiters(*part);
          session->participants.push_back(part);
        }

        (void)WriteJson(stream, {{"v", 1}, {"ok", true}, {"op", "attach"}});
        (void)sm.Apply(MediaRelayAttachEvent::AttachOk);
        StartParticipantAsync(session, part);
        return;
      } else {
        RejectAndCloseAttach(sm, stream, "unsupported op", MediaRelayAttachEvent::OpUnsupported);
        return;
      }
    }
  }
};

MediaRelayService::MediaRelayService(Libp2pHost& host, PeerSessionManager& sessions)
    : impl_(std::make_shared<Impl>()), host_(host), sessions_(sessions) {
  impl_->host = &host_;
  impl_->sessions = &sessions_;
}

MediaRelayService::~MediaRelayService() {
  Stop();
}

void MediaRelayService::Start() {
  if (started_ || !host_.IsRunning()) {
    return;
  }
  started_ = true;
  auto impl = impl_;
  host_.GetHost().setProtocolHandler({ProtocolName{kMediaRelayProtocolId}},
                                     [impl](libp2p::StreamAndProtocol stream) {
                                       impl->HandleInbound(std::move(stream));
                                     });
}

void MediaRelayService::Stop() {
  started_ = false;
  Detach();
}

void MediaRelayService::SetBudget(const MediaRelayBudgetConfig& budget) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->budget = budget;
}

void MediaRelayService::SetPricing(const RelayPricingConfig& pricing) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->pricing = pricing;
}

void MediaRelayService::SetAdmissionPolicy(MediaRelayAdmissionPolicy policy) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->admission = std::move(policy);
}

Roe<MediaRelayQuote> MediaRelayService::RequestQuote(const std::string& hop_peer_key,
                                                     const MediaRelayQuoteRequest& request,
                                                     int timeout_ms) {
  if (!host_.IsRunning()) {
    return Error("media-relay host not running");
  }
  if (!sessions_.IsDialable(hop_peer_key)) {
    return Error("hop peer endpoint not registered");
  }

  nlohmann::json req = {{"v", 1},
                        {"op", "quote"},
                        {"call_id", request.call_id},
                        {"participants", request.participants},
                        {"want_up_bps", request.want_up_bps},
                        {"want_down_bps", request.want_down_bps}};

  auto result_promise = std::make_shared<std::promise<Roe<MediaRelayQuote>>>();
  auto result_future = result_promise->get_future();
  auto settled = std::make_shared<std::atomic<bool>>(false);

  const bool circuit_backed = sessions_.IsCircuitBacked(hop_peer_key);

  sessions_.OpenStream(hop_peer_key, {ProtocolName{kMediaRelayProtocolId}},
                       [req = std::move(req), result_promise, settled, circuit_backed, &host = host_](
                           libp2p::StreamAndProtocolOrError stream_res) {
                         PostLibp2pWorker(host, WorkerLane::Normal,
                                                   [req, result_promise, settled, circuit_backed,
                                                    stream_res = std::move(stream_res)]() mutable {
                           auto finish = [&](Roe<MediaRelayQuote> value) {
                             if (!settled->exchange(true)) {
                               result_promise->set_value(std::move(value));
                             }
                           };
                           if (!stream_res) {
                             const auto& ec = stream_res.error();
                             finish(Error(std::string("media-relay stream open failed: ") + ec.message()));
                             return;
                           }
                           auto stream = std::move(stream_res.value().stream);
                           if (!WriteJson(stream, req)) {
                             finish(Error("Failed to send quote"));
                             if (!circuit_backed) {
                               stream->close([](auto&&) {});
                             }
                             return;
                           }
                           auto root = ReadJson(stream);
                           if (!circuit_backed) {
                             stream->close([](auto&&) {});
                           }
                           if (!root) {
                             finish(root.error());
                             return;
                           }
                           MediaRelayQuote q;
                           q.ok = root->value("ok", false);
                           q.error = root->value("error", "");
                           q.quote_id = root->value("quote_id", "");
                           q.a_up_bps = root->value("A_up", static_cast<int64_t>(0));
                           q.a_down_bps = root->value("A_down", static_cast<int64_t>(0));
                           q.b_up_bps = root->value("B_up", static_cast<int64_t>(0));
                           q.b_down_bps = root->value("B_down", static_cast<int64_t>(0));
                           q.pricing_mode = root->value("mode", "volunteer");
                           q.rate = root->value("rate", 0.0);
                           q.ceiling_bytes = root->value("ceiling_bytes", static_cast<int64_t>(0));
                           q.ceiling_amount = root->value("ceiling_amount", 0.0);
                           finish(q);
                         });
                       });

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  if (result_future.wait_for(std::chrono::milliseconds(wait_ms)) != std::future_status::ready) {
    settled->exchange(true);
    return Error(std::string("media-relay quote timed out (hop=") + hop_peer_key + ")");
  }
  return result_future.get();
}

Roe<MediaRelayAttachResult> MediaRelayService::AcceptAndAttach(
    const std::string& hop_peer_key, const std::string& quote_id, const std::string& call_id,
    const std::string& auth_stub, std::function<void(MediaDataFrame)> on_frame, int timeout_ms) {
  if (!host_.IsRunning()) {
    return Error("media-relay host not running");
  }
  if (!sessions_.IsDialable(hop_peer_key)) {
    return Error("hop peer endpoint not registered");
  }

  // Detach-then-attach (s1/N026): abort prior waiter and tear down prior session.
  Detach();

  auto result_promise = std::make_shared<std::promise<Roe<MediaRelayAttachResult>>>();
  auto result_future = result_promise->get_future();
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto impl = impl_;

  {
    std::lock_guard<std::mutex> lock(impl->mu);
    impl->client_attach_settled = settled;
    impl->client_attach_promise = result_promise;
    (void)impl->ApplyClientLocked(MediaRelayClientEvent::AttachRequested, call_id);
  }

  sessions_.OpenStream(
      hop_peer_key, {ProtocolName{kMediaRelayProtocolId}},
      [impl, quote_id, call_id, auth_stub, on_frame = std::move(on_frame), settled,
       &host = host_](libp2p::StreamAndProtocolOrError stream_res) mutable {
        PostLibp2pWorker(host, WorkerLane::Normal,
                         [impl, quote_id, call_id, auth_stub, on_frame = std::move(on_frame), settled,
                          stream_res = std::move(stream_res)]() mutable {
                           auto finish = [&](Roe<MediaRelayAttachResult> value,
                                             MediaRelayClientEvent ev) {
                             std::lock_guard<std::mutex> lock(impl->mu);
                             if (!value) {
                               (void)impl->ApplyClientLocked(ev, call_id);
                             }
                             impl->CompleteClientAttachLocked(std::move(value));
                           };
                           if (settled->load(std::memory_order_acquire)) {
                             if (stream_res) {
                               stream_res.value().stream->close([](auto&&) {});
                             }
                             return;
                           }
                           if (!stream_res) {
                             const auto& ec = stream_res.error();
                             finish(Error(std::string("media-relay stream open failed: ") + ec.message()),
                                    MediaRelayClientEvent::OpenStreamFail);
                             return;
                           }
                           auto stream = std::move(stream_res.value().stream);
                           {
                             std::lock_guard<std::mutex> lock(impl->mu);
                             if (settled->load(std::memory_order_acquire) ||
                                 !impl->ApplyClientLocked(MediaRelayClientEvent::OpenStreamOk, call_id)) {
                               stream->close([](auto&&) {});
                               return;
                             }
                           }
                           if (!WriteJson(stream, {{"v", 1}, {"op", "accept"}, {"quote_id", quote_id}})) {
                             finish(Error("Failed to send accept"), MediaRelayClientEvent::AcceptFail);
                             stream->close([](auto&&) {});
                             return;
                           }
                           auto accept_root = ReadJson(stream);
                           if (settled->load(std::memory_order_acquire)) {
                             stream->close([](auto&&) {});
                             return;
                           }
                           if (!accept_root || !accept_root->value("ok", false)) {
                             finish(Error(accept_root ? accept_root->value("error", "accept failed")
                                                      : accept_root.error().message),
                                    MediaRelayClientEvent::AcceptFail);
                             stream->close([](auto&&) {});
                             return;
                           }
                           const std::string token = accept_root->value("session_token", "");
                           {
                             std::lock_guard<std::mutex> lock(impl->mu);
                             if (settled->load(std::memory_order_acquire) ||
                                 !impl->ApplyClientLocked(MediaRelayClientEvent::AcceptOk, call_id)) {
                               stream->close([](auto&&) {});
                               return;
                             }
                           }
                           if (!WriteJson(stream, {{"v", 1},
                                                   {"op", "attach"},
                                                   {"session_token", token},
                                                   {"call_id", call_id},
                                                   {"auth", auth_stub}})) {
                             finish(Error("Failed to send attach"), MediaRelayClientEvent::AttachFail);
                             stream->close([](auto&&) {});
                             return;
                           }
                           auto attach_root = ReadJson(stream);
                           if (settled->load(std::memory_order_acquire)) {
                             stream->close([](auto&&) {});
                             return;
                           }
                           if (!attach_root || !attach_root->value("ok", false)) {
                             finish(Error(attach_root ? attach_root->value("error", "attach failed")
                                                      : attach_root.error().message),
                                    MediaRelayClientEvent::AttachFail);
                             stream->close([](auto&&) {});
                             return;
                           }

                           {
                             std::lock_guard<std::mutex> lock(impl->mu);
                             // Bug fix: never install client_stream after timeout/Detach settled.
                             if (settled->load(std::memory_order_acquire) ||
                                 impl->ClientPhase() == MediaRelayClientPhase::Idle ||
                                 impl->ClientPhase() == MediaRelayClientPhase::Detaching ||
                                 !impl->ApplyClientLocked(MediaRelayClientEvent::AttachOk, call_id)) {
                               stream->close([](auto&&) {});
                               return;
                             }
                             impl->client_stream = stream;
                             impl->client_session_token = token;
                             impl->client_on_frame = std::move(on_frame);
                             impl->client_subscriptions.clear();
                             MediaRelayAttachResult out;
                             out.ok = true;
                             out.session_token = token;
                             impl->CompleteClientAttachLocked(out);
                           }
                           // Inbound reader starts later via StartClientFrameReader() after StartSfu.
                         });
      });

  // Slice the wait so Detach can complete the promise without blocking Leave for the full timeout.
  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  for (;;) {
    const auto status = result_future.wait_for(std::chrono::milliseconds(50));
    if (status == std::future_status::ready) {
      std::lock_guard<std::mutex> lock(impl->mu);
      if (impl->client_attach_settled == settled) {
        impl->client_attach_settled.reset();
        impl->client_attach_promise.reset();
      }
      return result_future.get();
    }
    {
      std::lock_guard<std::mutex> lock(impl->mu);
      if (impl->ClientPhase() == MediaRelayClientPhase::Attached && impl->client_stream) {
        settled->store(true, std::memory_order_release);
        if (impl->client_attach_settled == settled) {
          impl->client_attach_settled.reset();
          impl->client_attach_promise.reset();
        }
        MediaRelayAttachResult out;
        out.ok = true;
        out.session_token = impl->client_session_token;
        return out;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      settled->exchange(true);
      {
        std::lock_guard<std::mutex> lock(impl->mu);
        if (impl->client_attach_settled == settled) {
          impl->client_attach_settled.reset();
          impl->client_attach_promise.reset();
        }
        if (impl->ClientPhase() == MediaRelayClientPhase::Attached && impl->client_stream) {
          MediaRelayAttachResult out;
          out.ok = true;
          out.session_token = impl->client_session_token;
          return out;
        }
        // Drop any late-installed stream from a racing worker (should be rare after settled check).
        if (impl->ApplyClientLocked(MediaRelayClientEvent::AttachTimeout, call_id)) {
          impl->StopClientDuplexLocked();
          if (impl->client_stream) {
            impl->client_stream->close([](auto&&) {});
            impl->client_stream.reset();
          }
          impl->client_session_token.clear();
          impl->client_on_frame = nullptr;
          impl->client_subscriptions.clear();
        }
      }
      return Error(std::string("media-relay attach timed out (hop=") + hop_peer_key + ")");
    }
  }
}

void MediaRelayService::StartClientFrameReader() {
  if (!impl_) {
    return;
  }
  impl_->StartClientDuplex(impl_);
}

void MediaRelayService::SetClientTransportLostHandler(std::function<void()> handler) {
  if (!impl_) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->client_transport_lost_handler = std::move(handler);
}

Roe<MediaRelayAttachResult> MediaRelayService::AttachAsLocalHop(
    const std::string& call_id, std::function<void(MediaDataFrame)> on_frame) {
  if (call_id.empty()) {
    return Error("missing call_id");
  }
  if (!started_) {
    return Error("media_relay not started");
  }
  auto local_pid = LocalPeerIdBase58();
  if (!local_pid) {
    return local_pid.error();
  }

  // Same call already PreferLocal — refresh callback only (guest hop-hint thrash).
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->local_hop_part && impl_->local_hop_session &&
        impl_->local_hop_session->call_id == call_id && impl_->local_hop_peer_id == *local_pid) {
      impl_->local_hop_part->local_on_frame = std::move(on_frame);
      MediaRelayAttachResult out;
      out.ok = true;
      out.session_token = impl_->local_hop_session->session_token;
      logging::getLogger("MediaRelayService").info
          << "AttachAsLocalHop refresh call=" << call_id;
      return out;
    }
  }

  Detach();

  auto part = std::make_shared<HostParticipant>();
  part->peer_id = *local_pid;
  part->local_on_frame = std::move(on_frame);
  part->a_up_bps = OrDefault(impl_->budget.default_per_user_up_bps, kDefaultUserUpBps);
  part->a_down_bps = OrDefault(impl_->budget.default_per_user_down_bps, kDefaultUserDownBps);
  MediaRelayService::Impl::ConfigureParticipantLimiters(*part);

  std::shared_ptr<HostSession> session;
  std::string token;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto it = impl_->sessions_by_call.find(call_id);
    if (it != impl_->sessions_by_call.end()) {
      session = it->second;
      token = session->session_token;
      // Count after replacing prior local hop for same peer.
      size_t others = 0;
      for (const auto& p : session->participants) {
        if (p && !(p->peer_id == part->peer_id && p->local_on_frame)) {
          ++others;
        }
      }
      if (others >= MediaRelayService::kMaxParticipantsPerSession) {
        return Error("session participant limit");
      }
    } else {
      if (!impl_->CanOpenNewHostSessionLocked()) {
        return Error("host session limit");
      }
      session = std::make_shared<HostSession>();
      session->call_id = call_id;
      session->session_token = MakeId("s");
      session->b_up_bps = OrDefault(impl_->budget.max_session_up_bps, kDefaultSessionUpBps);
      session->b_down_bps = OrDefault(impl_->budget.max_session_down_bps, kDefaultSessionDownBps);
      session->ceiling_bytes = kDefaultCeilingBytes;
      MediaRelayService::Impl::ConfigureSessionLimiters(*session);
      token = session->session_token;
      impl_->sessions_by_call[call_id] = session;
      impl_->sessions_by_token[token] = session;
    }
    // Replace a prior local hop participant for this peer (re-attach).
    session->participants.erase(
        std::remove_if(session->participants.begin(), session->participants.end(),
                       [&](const std::shared_ptr<HostParticipant>& p) {
                         return p && p->peer_id == part->peer_id && p->local_on_frame;
                       }),
        session->participants.end());
    session->participants.push_back(part);
    impl_->local_hop_part = part;
    impl_->local_hop_session = session;
    impl_->local_hop_peer_id = part->peer_id;
    impl_->client_session_token = token;
  }

  MediaRelayAttachResult out;
  out.ok = true;
  out.session_token = token;
  return out;
}

Roe<void> MediaRelayService::Subscribe(uint32_t stream_id, uint16_t channel_id) {
  const uint64_t key = SubKey(stream_id, channel_id);
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->local_hop_part) {
      if (!impl_->local_hop_part->subscriptions.insert(key).second) {
        return {}; // already subscribed — avoid log/control spam
      }
      logging::getLogger("MediaRelayService").info
          << "local-hop subscribe stream=" << stream_id << " ch=" << channel_id
          << " call=" << (impl_->local_hop_session ? impl_->local_hop_session->call_id : "");
      return {};
    }
    if (impl_->client_subscriptions.count(key) != 0) {
      return {}; // already sent on this attach
    }
    if (!impl_->client_duplex) {
      return Error("not attached");
    }
    impl_->client_subscriptions.insert(key);
  }
  logging::getLogger("MediaRelayService").info
      << "client subscribe stream=" << stream_id << " ch=" << channel_id;
  const std::string json =
      nlohmann::json({{"v", 1}, {"op", "subscribe"}, {"stream_id", stream_id}, {"channel_id", channel_id}})
          .dump();
  if (!impl_->EnqueueClientBody(std::vector<uint8_t>(json.begin(), json.end()))) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->client_subscriptions.erase(key);
    return Error("not attached");
  }
  return {};
}

Roe<void> MediaRelayService::Unsubscribe(uint32_t stream_id, uint16_t channel_id) {
  const uint64_t key = SubKey(stream_id, channel_id);
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->local_hop_part) {
      impl_->local_hop_part->subscriptions.erase(key);
      return {};
    }
    impl_->client_subscriptions.erase(key);
    if (!impl_->client_duplex) {
      return Error("not attached");
    }
  }
  const std::string json =
      nlohmann::json({{"v", 1}, {"op", "unsubscribe"}, {"stream_id", stream_id}, {"channel_id", channel_id}})
          .dump();
  if (!impl_->EnqueueClientBody(std::vector<uint8_t>(json.begin(), json.end()))) {
    return Error("not attached");
  }
  return {};
}

Roe<void> MediaRelayService::SendFrame(const MediaDataFrame& frame) {
  std::shared_ptr<Stream> stream;
  std::shared_ptr<HostSession> session;
  std::string from_peer;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    stream = impl_->client_stream;
    if (impl_->local_hop_part) {
      session = impl_->local_hop_session;
      from_peer = impl_->local_hop_peer_id;
    }
  }
  if (session) {
    const std::vector<uint8_t> body = EncodeMediaDataFrame(frame);
    const int64_t nbytes = static_cast<int64_t>(body.size());
    const int64_t now_ms = util::NowUnixMs();
    {
      std::lock_guard<std::mutex> lock(impl_->mu);
      if (impl_->local_hop_part) {
        if (!impl_->local_hop_part->up_limiter.TryConsume(nbytes, now_ms) ||
            !session->session_up_limiter.TryConsume(nbytes, now_ms)) {
          ++impl_->local_hop_part->drops_rate;
          ++session->drops_total;
          ++session->drops_rate;
          return {}; // drop excess uplink; keep session
        }
        if (session->ceiling_bytes > 0 && session->bytes_total > session->ceiling_bytes) {
          ++session->drops_total;
          ++session->drops_ceiling;
          return {};
        }
        impl_->local_hop_part->bytes_up += nbytes;
        session->bytes_total += nbytes;
        session->bytes_up_window += nbytes;
      }
    }
    impl_->Fanout(session, from_peer, frame, body);
    return {};
  }
  if (!stream) {
    return Error("not attached");
  }
  if (!impl_->EnqueueClientBody(EncodeMediaDataFrame(frame))) {
    return Error("not attached");
  }
  return {};
}

void MediaRelayService::Detach() {
  std::shared_ptr<Stream> stream;
  std::shared_ptr<HostParticipant> local;
  std::shared_ptr<HostSession> local_session;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    const MediaRelayClientPhase prev = impl_->ClientPhase();
    if (prev != MediaRelayClientPhase::Idle) {
      impl_->SetClientPhaseLocked(MediaRelayClientPhase::Detaching,
                                  MediaRelayClientEvent::DetachRequested, impl_->client_call_id);
    }
    // Unblock AcceptAndAttach waiters (Leave / SoftMigrate / supersede).
    impl_->CompleteClientAttachLocked(Error("media-relay attach aborted"));
    impl_->StopClientDuplexLocked();
    stream = impl_->client_stream;
    impl_->client_stream.reset();
    impl_->client_session_token.clear();
    impl_->client_on_frame = nullptr;
    impl_->client_subscriptions.clear();
    local = impl_->local_hop_part;
    local_session = impl_->local_hop_session;
    impl_->local_hop_part.reset();
    impl_->local_hop_session.reset();
    impl_->local_hop_peer_id.clear();
    if (local) {
      local->local_on_frame = nullptr;
    }
    if (local && local_session) {
      local_session->participants.erase(
          std::remove_if(local_session->participants.begin(), local_session->participants.end(),
                         [&](const std::shared_ptr<HostParticipant>& p) {
                           return p.get() == local.get();
                         }),
          local_session->participants.end());
    }
    if (prev != MediaRelayClientPhase::Idle) {
      impl_->SetClientPhaseLocked(MediaRelayClientPhase::Idle, MediaRelayClientEvent::DetachRequested);
    }
  }
  impl_->client_reader_epoch.fetch_add(1, std::memory_order_acq_rel);
  if (stream) {
    // Close so in-flight async read/write complete and Leave can join capture.
    stream->close([](auto&&) {});
  }
}

bool MediaRelayService::IsAttached() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->client_stream != nullptr || impl_->local_hop_part != nullptr;
}

MediaRelayClientPhase MediaRelayService::ClientPhase() const {
  return impl_->ClientPhase();
}

bool MediaRelayService::IsLocalHopAttached() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->local_hop_part != nullptr;
}

double MediaRelayService::PathPressure() const {
  return HealthSnapshot().path_pressure;
}

CallHopHealth MediaRelayService::HealthSnapshot() const {
  CallHopHealth h;
  std::lock_guard<std::mutex> lock(impl_->mu);
  h.attached = impl_->local_hop_part != nullptr || impl_->client_stream != nullptr;
  const HostSession* session = nullptr;
  if (impl_->local_hop_session) {
    session = impl_->local_hop_session.get();
  }
  double max_fill = 0.0;
  if (session) {
    h.drops_total = session->drops_total;
    h.drops_rate = session->drops_rate;
    h.drops_queue = session->drops_queue;
    h.drops_ceiling = session->drops_ceiling;
    for (const auto& part : session->participants) {
      if (!part || part->local_on_frame) {
        continue;
      }
      CallHopPeerHealth peer;
      peer.peer_id = part->peer_id;
      peer.bytes_up = part->bytes_up;
      peer.bytes_down = part->bytes_down;
      peer.drops_queue = part->drops_queue;
      peer.drops_rate = part->drops_rate;
      if (part->duplex) {
        peer.outbound_backlog = part->duplex->OutboundBacklog();
      }
      const double fill =
          static_cast<double>(peer.outbound_backlog) / static_cast<double>(kMaxOutboundBacklog);
      max_fill = std::max(max_fill, fill);
      h.peers.push_back(std::move(peer));
    }
  }
  // Instantaneous duplex backlog fill — not lifetime drops_total/N.
  h.path_pressure = std::clamp(max_fill, 0.0, 1.0);
  return h;
}

} // namespace pbr
