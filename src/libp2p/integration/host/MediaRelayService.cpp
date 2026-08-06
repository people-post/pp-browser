#include "libp2p/integration/host/MediaRelayService.h"

#include "base/media/ByteRateLimiter.h"
#include "base/people/RelayScope.h"
#include "common/Logger.h"
#include "common/Utilities.h"
#include "libp2p/integration/host/Libp2pWorker.h"
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
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

namespace pbr {

namespace {

using libp2p::Bytes;
using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

constexpr uint8_t kMediaDataVersion = 1;
constexpr size_t kMediaDataHeaderBytes = 1 + 4 + 2 + 1 + 4 + 1; // ver+stream+chan+type+seq+mark
constexpr size_t kMaxMediaFrameBytes = 256 * 1024;

/** Soft defaults when budget fields are 0 (unbounded / ops default). */
constexpr int64_t kDefaultUserUpBps = 500'000;
constexpr int64_t kDefaultUserDownBps = 2'000'000;
constexpr int64_t kDefaultSessionUpBps = 4'000'000;
constexpr int64_t kDefaultSessionDownBps = 16'000'000;
constexpr int64_t kDefaultCeilingBytes = 50'000'000;
/** Per-subscriber outbound write backlog (frames); drop-oldest when full (V032). */
constexpr size_t kMaxOutboundPendingFrames = 48;

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
  std::atomic<size_t> outbound_pending{0};
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
};

struct PendingQuote {
  MediaRelayQuote quote;
  std::string call_id;
};

} // namespace

std::vector<uint8_t> EncodeMediaDataFrame(const MediaDataFrame& frame) {
  std::vector<uint8_t> body(kMediaDataHeaderBytes + frame.payload.size());
  size_t i = 0;
  body[i++] = kMediaDataVersion;
  auto put_u32 = [&](uint32_t v) {
    body[i++] = static_cast<uint8_t>((v >> 24) & 0xff);
    body[i++] = static_cast<uint8_t>((v >> 16) & 0xff);
    body[i++] = static_cast<uint8_t>((v >> 8) & 0xff);
    body[i++] = static_cast<uint8_t>(v & 0xff);
  };
  auto put_u16 = [&](uint16_t v) {
    body[i++] = static_cast<uint8_t>((v >> 8) & 0xff);
    body[i++] = static_cast<uint8_t>(v & 0xff);
  };
  put_u32(frame.stream_id);
  put_u16(frame.channel_id);
  body[i++] = static_cast<uint8_t>(frame.channel_type);
  put_u32(frame.seq);
  body[i++] = frame.mark;
  if (!frame.payload.empty()) {
    std::memcpy(body.data() + i, frame.payload.data(), frame.payload.size());
  }
  return body;
}

Roe<MediaDataFrame> DecodeMediaDataFrame(const std::vector<uint8_t>& body) {
  if (body.size() < kMediaDataHeaderBytes) {
    return Error("media data frame too short");
  }
  if (body[0] != kMediaDataVersion) {
    return Error("unsupported media data version");
  }
  auto get_u32 = [&](size_t at) -> uint32_t {
    return (static_cast<uint32_t>(body[at]) << 24) | (static_cast<uint32_t>(body[at + 1]) << 16) |
           (static_cast<uint32_t>(body[at + 2]) << 8) | static_cast<uint32_t>(body[at + 3]);
  };
  auto get_u16 = [&](size_t at) -> uint16_t {
    return static_cast<uint16_t>((static_cast<uint16_t>(body[at]) << 8) | body[at + 1]);
  };
  MediaDataFrame frame;
  frame.stream_id = get_u32(1);
  frame.channel_id = get_u16(5);
  frame.channel_type = static_cast<MediaChannelType>(body[7]);
  frame.seq = get_u32(8);
  frame.mark = body[12];
  frame.payload.assign(body.begin() + static_cast<std::ptrdiff_t>(kMediaDataHeaderBytes), body.end());
  return frame;
}

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

  // Client attach state (remote hop dial)
  std::shared_ptr<Stream> client_stream;
  std::mutex client_write_mu; // Subscribe/SendFrame/Detach vs each other — sync path only
  std::string client_session_token;
  std::function<void(MediaDataFrame)> client_on_frame;
  /** Bumped on Detach — stale readers exit without clearing a newer attach's epoch. */
  std::atomic<uint64_t> client_reader_epoch{0};
  std::atomic<uint64_t> client_reader_started_for{0};

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
    if (!call_id.empty() && sessions_by_call.find(call_id) != sessions_by_call.end()) {
      return true;
    }
    return AdmitPeer(peer_id);
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
      bool subscribed = false;
      bool over_ceiling = false;
      bool rate_limited = false;
      bool queue_full = false;
      std::function<void(MediaDataFrame)> on_frame;
      std::shared_ptr<Stream> out_stream;
      std::weak_ptr<HostParticipant> weak_part = part;
      {
        std::lock_guard<std::mutex> lock(mu);
        local = static_cast<bool>(part->local_on_frame);
        if (!local && !part->stream) {
          continue;
        }
        subscribed = part->subscriptions.find(key) != part->subscriptions.end();
        if (!subscribed) {
          continue;
        }
        if (frame.channel_type == MediaChannelType::LatestLossy) {
          auto it = part->last_lossy_seq.find(key);
          if (it != part->last_lossy_seq.end() && frame.seq < it->second && frame.mark == 0) {
            continue;
          }
          part->last_lossy_seq[key] = frame.seq;
        }
        over_ceiling = session->ceiling_bytes > 0 && session->bytes_total > session->ceiling_bytes;
        if (over_ceiling) {
          ++session->drops_total;
          continue;
        }
        if (!part->down_limiter.TryConsume(nbytes, now_ms) ||
            !session->session_down_limiter.TryConsume(nbytes, now_ms)) {
          ++part->drops_rate;
          ++session->drops_total;
          rate_limited = true;
          continue;
        }
        if (!local && part->outbound_pending.load(std::memory_order_relaxed) >= kMaxOutboundPendingFrames) {
          ++part->drops_queue;
          ++session->drops_total;
          queue_full = true;
          continue;
        }
        part->bytes_down += nbytes;
        session->bytes_total += nbytes;
        session->bytes_down_window += nbytes;
        if (local) {
          on_frame = part->local_on_frame;
        } else {
          out_stream = part->stream;
          part->outbound_pending.fetch_add(1, std::memory_order_relaxed);
        }
      }
      if (over_ceiling || rate_limited || queue_full) {
        continue;
      }
      if (local) {
        if (on_frame) {
          on_frame(frame);
        }
        continue;
      }
      if (out_stream && host) {
        PostLibp2pWorker(*host, WorkerLane::Normal, [out_stream, body, weak_part]() {
          (void)BlockingWriteLengthPrefixedFrame(out_stream, body);
          if (auto p = weak_part.lock()) {
            auto cur = p->outbound_pending.load(std::memory_order_relaxed);
            while (cur > 0 &&
                   !p->outbound_pending.compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed)) {
            }
          }
        });
      }
    }
  }

  /** Returns false when the participant session should end. */
  bool ProcessParticipantFrame(const std::shared_ptr<HostSession>& session,
                               const std::shared_ptr<HostParticipant>& part,
                               const std::vector<uint8_t>& body) {
    if (body.empty()) {
      return false;
    }
    if (body[0] == '{') {
      nlohmann::json root =
          nlohmann::json::parse(std::string(body.begin(), body.end()), nullptr, false);
      if (root.is_discarded() || !root.is_object()) {
        return false;
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
      return false;
    }
    const int64_t nbytes = static_cast<int64_t>(body.size());
    const int64_t now_ms = util::NowUnixMs();
    {
      std::lock_guard<std::mutex> lock(mu);
      if (!part->up_limiter.TryConsume(nbytes, now_ms) ||
          !session->session_up_limiter.TryConsume(nbytes, now_ms)) {
        ++part->drops_rate;
        ++session->drops_total;
        return true; // drop excess uplink; keep session
      }
      if (session->ceiling_bytes > 0 && session->bytes_total > session->ceiling_bytes) {
        ++session->drops_total;
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
    return sessions_by_call.size() < MediaRelayService::kMaxHostSessions;
  }

  static bool CanAddParticipantLocked(const HostSession& session) {
    return session.participants.size() < MediaRelayService::kMaxParticipantsPerSession;
  }

  void CleanupParticipant(const std::shared_ptr<HostSession>& session,
                          const std::shared_ptr<HostParticipant>& part) {
    if (part->duplex_cancelled) {
      part->duplex_cancelled->store(true, std::memory_order_release);
    }
    if (part->duplex) {
      part->duplex->Stop();
      part->duplex.reset();
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
          [self, session, part]() { self->CleanupParticipant(session, part); });
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
      std::string remote;
      if (auto peer = stream->remotePeerId()) {
        remote = peer.value().toBase58();
      }

      // Admission runs per control op (quote/accept/attach) with call_id so joiners can
      // attach on a fresh stream after an admitted sponsor opened the session.
      // Control handshake: quote → accept → attach (may be multi-message / multi-stream)
      std::string accepted_quote_id;
      std::string session_token;
      std::shared_ptr<HostSession> session;

      while (!session) {
        auto root = ReadJson(stream);
        if (!root) {
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
          {
            std::lock_guard<std::mutex> lock(mu);
            if (!AdmitPeerForCall(remote, req.call_id)) {
              (void)WriteJson(stream,
                              {{"v", 1}, {"ok", false}, {"error", "prefer contacts: stranger refused"}});
              stream->close([](auto&&) {});
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
        } else if (op == "accept") {
          const std::string quote_id = root->value("quote_id", "");
          PendingQuote pending;
          {
            std::lock_guard<std::mutex> lock(mu);
            auto it = quotes_by_id.find(quote_id);
            if (it == quotes_by_id.end()) {
              (void)WriteJson(stream, {{"v", 1}, {"ok", false}, {"error", "unknown quote"}});
              stream->close([](auto&&) {});
              return;
            }
            pending = it->second;
            if (!AdmitPeerForCall(remote, pending.call_id)) {
              (void)WriteJson(stream,
                              {{"v", 1}, {"ok", false}, {"error", "prefer contacts: stranger refused"}});
              stream->close([](auto&&) {});
              return;
            }
            quotes_by_id.erase(it);
          }
          accepted_quote_id = quote_id;
          session_token = MakeId("s");
          (void)WriteJson(stream, {{"v", 1},
                                   {"ok", true},
                                   {"op", "accept"},
                                   {"session_token", session_token},
                                   {"quote_id", accepted_quote_id}});
        } else if (op == "attach") {
          const std::string token = root->value("session_token", session_token);
          const std::string call_id = root->value("call_id", "");
          const std::string auth = root->value("auth", "");
          if (token.empty() || call_id.empty()) {
            (void)WriteJson(stream, {{"v", 1}, {"ok", false}, {"error", "missing session_token or call_id"}});
            stream->close([](auto&&) {});
            return;
          }
          // Auth stub: non-empty auth required; must equal call_id for v1 dogfood.
          if (auth.empty() || auth != call_id) {
            (void)WriteJson(stream, {{"v", 1}, {"ok", false}, {"error", "auth failed"}});
            stream->close([](auto&&) {});
            return;
          }
          {
            std::lock_guard<std::mutex> lock(mu);
            if (!AdmitPeerForCall(remote, call_id)) {
              (void)WriteJson(stream,
                              {{"v", 1}, {"ok", false}, {"error", "prefer contacts: stranger refused"}});
              stream->close([](auto&&) {});
              return;
            }
          }

          auto part = std::make_shared<HostParticipant>();
          part->peer_id = remote;
          part->stream = stream;

          {
            std::lock_guard<std::mutex> lock(mu);
            auto it = sessions_by_call.find(call_id);
            if (it != sessions_by_call.end()) {
              session = it->second;
              if (!CanAddParticipantLocked(*session)) {
                (void)WriteJson(stream, {{"v", 1}, {"ok", false}, {"error", "session participant limit"}});
                stream->close([](auto&&) {});
                return;
              }
            } else {
              if (!CanOpenNewHostSessionLocked()) {
                (void)WriteJson(stream, {{"v", 1}, {"ok", false}, {"error", "host session limit"}});
                stream->close([](auto&&) {});
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
          StartParticipantAsync(session, part);
          return;
        } else {
          (void)WriteJson(stream, {{"v", 1}, {"ok", false}, {"error", "unsupported op"}});
          stream->close([](auto&&) {});
          return;
        }
      }
  }

  void StartClientReader(const std::shared_ptr<Impl>& self) {
    if (!host) {
      return;
    }
    const uint64_t epoch = client_reader_epoch.load(std::memory_order_acquire);
    uint64_t started = client_reader_started_for.load(std::memory_order_acquire);
    while (started != epoch) {
      if (client_reader_started_for.compare_exchange_weak(started, epoch,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
        // Client inbound stays on worker (sync) — async hop pump only; avoids same-stream
        // concurrent read/write on Yamux when SendFrame/Subscribe post from app threads.
        PostLibp2pWorker(*host, WorkerLane::Normal,
                         [self, epoch]() { self->RunClientReader(epoch); });
        return;
      }
      if (started == epoch) {
        return;
      }
      if (client_reader_epoch.load(std::memory_order_acquire) != epoch) {
        return;
      }
    }
  }

  void RunClientReader(uint64_t epoch) {
    while (client_reader_epoch.load(std::memory_order_acquire) == epoch) {
      std::shared_ptr<Stream> stream;
      std::function<void(MediaDataFrame)> cb;
      {
        std::lock_guard<std::mutex> lock(mu);
        stream = client_stream;
        cb = client_on_frame;
      }
      if (!stream) {
        break;
      }
      auto body = ReadExactFrame(stream);
      if (client_reader_epoch.load(std::memory_order_acquire) != epoch) {
        break;
      }
      if (!body || body->empty()) {
        break;
      }
      if ((*body)[0] == '{') {
        continue;
      }
      auto frame = DecodeMediaDataFrame(*body);
      if (!frame) {
        break;
      }
      if (cb) {
        cb(*frame);
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
  Detach();

  auto result_promise = std::make_shared<std::promise<Roe<MediaRelayAttachResult>>>();
  auto result_future = result_promise->get_future();
  auto settled = std::make_shared<std::atomic<bool>>(false);
  auto impl = impl_;

  sessions_.OpenStream(
      hop_peer_key, {ProtocolName{kMediaRelayProtocolId}},
      [impl, quote_id, call_id, auth_stub, on_frame = std::move(on_frame), result_promise, settled, &host = host_](
          libp2p::StreamAndProtocolOrError stream_res) mutable {
        PostLibp2pWorker(host, WorkerLane::Normal,
                                  [impl, quote_id, call_id, auth_stub, on_frame = std::move(on_frame), result_promise,
                                   settled, stream_res = std::move(stream_res)]() mutable {
          auto finish = [&](Roe<MediaRelayAttachResult> value) {
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
          if (!WriteJson(stream, {{"v", 1}, {"op", "accept"}, {"quote_id", quote_id}})) {
            finish(Error("Failed to send accept"));
            stream->close([](auto&&) {});
            return;
          }
          auto accept_root = ReadJson(stream);
          if (!accept_root || !accept_root->value("ok", false)) {
            finish(Error(accept_root ? accept_root->value("error", "accept failed")
                                     : accept_root.error().message));
            stream->close([](auto&&) {});
            return;
          }
          const std::string token = accept_root->value("session_token", "");
          if (!WriteJson(stream, {{"v", 1},
                                  {"op", "attach"},
                                  {"session_token", token},
                                  {"call_id", call_id},
                                  {"auth", auth_stub}})) {
            finish(Error("Failed to send attach"));
            stream->close([](auto&&) {});
            return;
          }
          auto attach_root = ReadJson(stream);
          if (!attach_root || !attach_root->value("ok", false)) {
            finish(Error(attach_root ? attach_root->value("error", "attach failed")
                                     : attach_root.error().message));
            stream->close([](auto&&) {});
            return;
          }

          {
            std::lock_guard<std::mutex> lock(impl->mu);
            impl->client_stream = stream;
            impl->client_session_token = token;
            impl->client_on_frame = std::move(on_frame);
          }
          // Inbound reader starts later via StartClientFrameReader() after StartSfu.

          MediaRelayAttachResult out;
          out.ok = true;
          out.session_token = token;
          finish(out);
        });
      });

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  if (result_future.wait_for(std::chrono::milliseconds(wait_ms)) != std::future_status::ready) {
    settled->exchange(true);
    return Error(std::string("media-relay attach timed out (hop=") + hop_peer_key + ")");
  }
  return result_future.get();
}

void MediaRelayService::StartClientFrameReader() {
  if (!impl_) {
    return;
  }
  impl_->StartClientReader(impl_);
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
  std::shared_ptr<Stream> stream;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->local_hop_part) {
      impl_->local_hop_part->subscriptions.insert(SubKey(stream_id, channel_id));
      logging::getLogger("MediaRelayService").info
          << "local-hop subscribe stream=" << stream_id << " ch=" << channel_id
          << " call=" << (impl_->local_hop_session ? impl_->local_hop_session->call_id : "");
      return {};
    }
    stream = impl_->client_stream;
  }
  if (!stream) {
    return Error("not attached");
  }
  logging::getLogger("MediaRelayService").info
      << "client subscribe stream=" << stream_id << " ch=" << channel_id;
  std::lock_guard<std::mutex> wlock(impl_->client_write_mu);
  return WriteJson(stream, {{"v", 1}, {"op", "subscribe"}, {"stream_id", stream_id}, {"channel_id", channel_id}});
}

Roe<void> MediaRelayService::Unsubscribe(uint32_t stream_id, uint16_t channel_id) {
  std::shared_ptr<Stream> stream;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->local_hop_part) {
      impl_->local_hop_part->subscriptions.erase(SubKey(stream_id, channel_id));
      return {};
    }
    stream = impl_->client_stream;
  }
  if (!stream) {
    return Error("not attached");
  }
  std::lock_guard<std::mutex> wlock(impl_->client_write_mu);
  return WriteJson(stream,
                   {{"v", 1}, {"op", "unsubscribe"}, {"stream_id", stream_id}, {"channel_id", channel_id}});
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
          return {}; // drop excess uplink; keep session
        }
        if (session->ceiling_bytes > 0 && session->bytes_total > session->ceiling_bytes) {
          ++session->drops_total;
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
  const std::vector<uint8_t> encoded = EncodeMediaDataFrame(frame);
  std::lock_guard<std::mutex> wlock(impl_->client_write_mu);
  return WriteExactBody(stream, encoded);
}

void MediaRelayService::Detach() {
  std::shared_ptr<Stream> stream;
  std::shared_ptr<HostParticipant> local;
  std::shared_ptr<HostSession> local_session;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    stream = impl_->client_stream;
    impl_->client_stream.reset();
    impl_->client_session_token.clear();
    impl_->client_on_frame = nullptr;
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
                         [&](const std::shared_ptr<HostParticipant>& p) { return p.get() == local.get(); }),
          local_session->participants.end());
    }
  }
  impl_->client_reader_epoch.fetch_add(1, std::memory_order_acq_rel);
  if (stream) {
    {
      std::lock_guard<std::mutex> wlock(impl_->client_write_mu);
      (void)WriteJson(stream, {{"v", 1}, {"op", "detach"}});
    }
    stream->close([](auto&&) {});
  }
}

bool MediaRelayService::IsAttached() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->client_stream != nullptr || impl_->local_hop_part != nullptr;
}

bool MediaRelayService::IsLocalHopAttached() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->local_hop_part != nullptr;
}

double MediaRelayService::PathPressure() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  uint64_t drops = 0;
  if (impl_->local_hop_session) {
    drops = impl_->local_hop_session->drops_total;
  }
  // Soft map: 0 drops → 0; ~40 drops → ~1.
  return std::clamp(static_cast<double>(drops) / 40.0, 0.0, 1.0);
}

} // namespace pbr
