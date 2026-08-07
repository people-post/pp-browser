#pragma once

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

namespace media_relay_detail {

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

} // namespace media_relay_detail

using media_relay_detail::HostParticipant;
using media_relay_detail::HostSession;
using media_relay_detail::PendingQuote;
using media_relay_detail::ProtocolName;
using media_relay_detail::Stream;
using media_relay_detail::MediaRelayLog;
using media_relay_detail::MediaDataFrameConfig;
using media_relay_detail::ReadExactFrame;
using media_relay_detail::WriteExactBody;
using media_relay_detail::WriteJson;
using media_relay_detail::ReadJson;
using media_relay_detail::RejectAndCloseAttach;
using media_relay_detail::OrDefault;
using media_relay_detail::MakeId;
using media_relay_detail::SubKey;
using media_relay_detail::kDefaultUserUpBps;
using media_relay_detail::kDefaultUserDownBps;
using media_relay_detail::kDefaultSessionUpBps;
using media_relay_detail::kDefaultSessionDownBps;
using media_relay_detail::kDefaultCeilingBytes;
using media_relay_detail::kMaxOutboundBacklog;

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

  void HandleInbound(libp2p::StreamAndProtocol stream_and_protocol);
  void HandleInboundBody(std::shared_ptr<Stream> stream);

  /** Cancel all HostSession participants and drop quote/session maps (Service Stop). */
  void ShutdownHostSessions() {
    std::vector<std::pair<std::shared_ptr<HostSession>, std::shared_ptr<HostParticipant>>> to_clean;
    {
      std::lock_guard<std::mutex> lock(mu);
      for (auto& [_, session] : sessions_by_call) {
        if (!session) {
          continue;
        }
        for (const auto& part : session->participants) {
          if (part) {
            to_clean.emplace_back(session, part);
          }
        }
      }
      quotes_by_id.clear();
      sessions_by_token.clear();
      sessions_by_call.clear();
    }
    for (auto& [session, part] : to_clean) {
      CleanupParticipant(session, part, "service_stop");
    }
  }

};

} // namespace pbr
