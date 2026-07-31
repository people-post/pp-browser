#include "libp2p/integration/host/MediaRelayService.h"

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
#include <thread>
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

Roe<std::vector<uint8_t>> ReadExactFrame(const std::shared_ptr<Stream>& stream) {
  Bytes header(8);
  std::promise<outcome::result<void>> header_promise;
  auto header_future = header_promise.get_future();
  libp2p::read(stream, header, [&](outcome::result<void> result) { header_promise.set_value(result); });
  if (!header_future.get()) {
    return Error("Failed to read media-relay frame header");
  }
  uint64_t payload_len = 0;
  for (size_t i = 0; i < 8; ++i) {
    payload_len = (payload_len << 8) | header[i];
  }
  if (payload_len > kMaxMediaFrameBytes) {
    return Error("media-relay frame too large");
  }
  Bytes payload(payload_len);
  std::promise<outcome::result<void>> body_promise;
  auto body_future = body_promise.get_future();
  libp2p::read(stream, payload, [&](outcome::result<void> result) { body_promise.set_value(result); });
  if (!body_future.get()) {
    return Error("Failed to read media-relay frame body");
  }
  return std::vector<uint8_t>(payload.begin(), payload.end());
}

Roe<void> WriteExactBody(const std::shared_ptr<Stream>& stream, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> frame(8 + body.size());
  uint64_t len = body.size();
  for (int i = 7; i >= 0; --i) {
    frame[static_cast<size_t>(i)] = static_cast<uint8_t>(len & 0xff);
    len >>= 8;
  }
  std::memcpy(frame.data() + 8, body.data(), body.size());
  std::promise<outcome::result<void>> write_promise;
  auto write_future = write_promise.get_future();
  libp2p::write(stream, libp2p::Bytes(frame), [&](outcome::result<void> result) { write_promise.set_value(result); });
  if (!write_future.get()) {
    return Error("Failed to write media-relay frame");
  }
  return {};
}

Roe<void> WriteJson(const std::shared_ptr<Stream>& stream, const nlohmann::json& root) {
  auto encoded = EncodeStreamJsonFrame(root.dump());
  if (!encoded) {
    return encoded.error();
  }
  // EncodeStreamJsonFrame already includes length prefix — write raw.
  std::promise<outcome::result<void>> write_promise;
  auto write_future = write_promise.get_future();
  libp2p::write(stream, libp2p::Bytes(*encoded),
                [&](outcome::result<void> result) { write_promise.set_value(result); });
  if (!write_future.get()) {
    return Error("Failed to write media-relay json");
  }
  return {};
}

Roe<nlohmann::json> ReadJson(const std::shared_ptr<Stream>& stream) {
  Bytes header(8);
  std::promise<outcome::result<void>> header_promise;
  auto header_future = header_promise.get_future();
  libp2p::read(stream, header, [&](outcome::result<void> result) { header_promise.set_value(result); });
  if (!header_future.get()) {
    return Error("Failed to read media-relay json header");
  }
  std::vector<uint8_t> frame(header.begin(), header.end());
  uint64_t payload_len = 0;
  for (size_t i = 0; i < 8; ++i) {
    payload_len = (payload_len << 8) | frame[i];
  }
  if (payload_len > kMaxStreamJsonFrameBytes) {
    return Error("media-relay json too large");
  }
  Bytes payload(payload_len);
  std::promise<outcome::result<void>> body_promise;
  auto body_future = body_promise.get_future();
  libp2p::read(stream, payload, [&](outcome::result<void> result) { body_promise.set_value(result); });
  if (!body_future.get()) {
    return Error("Failed to read media-relay json body");
  }
  frame.insert(frame.end(), payload.begin(), payload.end());
  auto json_utf8 = DecodeStreamJsonFrame(frame);
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
  std::mutex write_mu; // serializes hop→client writes (fanout vs control acks)
  std::unordered_set<uint64_t> subscriptions;
  std::unordered_map<uint64_t, uint32_t> last_lossy_seq;
  int64_t a_up_bps = 0;
  int64_t a_down_bps = 0;
  int64_t bytes_up = 0;
  int64_t bytes_down = 0;
};

struct HostSession {
  std::string call_id;
  std::string session_token;
  int64_t b_up_bps = 0;
  int64_t b_down_bps = 0;
  int64_t ceiling_bytes = 0;
  int64_t bytes_total = 0;
  std::vector<std::shared_ptr<HostParticipant>> participants;
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
  PeerSessionManager* sessions = nullptr;
  MediaRelayBudgetConfig budget;
  RelayPricingConfig pricing;
  MediaRelayAdmissionPolicy admission;

  std::unordered_map<std::string, PendingQuote> quotes_by_id;
  std::unordered_map<std::string, std::shared_ptr<HostSession>> sessions_by_token;
  std::unordered_map<std::string, std::shared_ptr<HostSession>> sessions_by_call;

  // Client attach state
  std::shared_ptr<Stream> client_stream;
  std::mutex client_write_mu; // Subscribe/SendFrame/Detach vs each other
  std::string client_session_token;
  std::function<void(MediaDataFrame)> client_on_frame;
  std::atomic<bool> client_reader_running{false};

  bool AdmitPeer(const std::string& peer_id) {
    if (!admission.prefer_contacts_only || admission.contact_peer_ids.empty()) {
      return true;
    }
    return !peer_id.empty() && admission.contact_peer_ids.count(peer_id) > 0;
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
    const uint64_t key = SubKey(frame.stream_id, frame.channel_id);
    for (const auto& part : session->participants) {
      if (!part || !part->stream || part->peer_id == from_peer) {
        continue;
      }
      if (part->subscriptions.find(key) == part->subscriptions.end()) {
        continue;
      }
      if (frame.channel_type == MediaChannelType::LatestLossy) {
        auto it = part->last_lossy_seq.find(key);
        if (it != part->last_lossy_seq.end() && frame.seq < it->second && frame.mark == 0) {
          continue; // stale under lossy policy
        }
        part->last_lossy_seq[key] = frame.seq;
      }
      part->bytes_down += static_cast<int64_t>(body.size());
      session->bytes_total += static_cast<int64_t>(body.size());
      if (session->ceiling_bytes > 0 && session->bytes_total > session->ceiling_bytes) {
        continue;
      }
      std::lock_guard<std::mutex> wlock(part->write_mu);
      (void)WriteExactBody(part->stream, body);
    }
  }

  void HandleParticipantLoop(std::shared_ptr<HostSession> session, std::shared_ptr<HostParticipant> part) {
    while (true) {
      auto body = ReadExactFrame(part->stream);
      if (!body) {
        break;
      }
      if (body->empty()) {
        break;
      }
      if ((*body)[0] == '{') {
        nlohmann::json root =
            nlohmann::json::parse(std::string(body->begin(), body->end()), nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
          break;
        }
        const std::string op = root.value("op", "");
        if (op == "subscribe") {
          part->subscriptions.insert(SubKey(root.value("stream_id", 0u),
                                            static_cast<uint16_t>(root.value("channel_id", 0))));
          std::lock_guard<std::mutex> wlock(part->write_mu);
          (void)WriteJson(part->stream, {{"v", 1}, {"ok", true}, {"op", "subscribe"}});
        } else if (op == "unsubscribe") {
          part->subscriptions.erase(SubKey(root.value("stream_id", 0u),
                                           static_cast<uint16_t>(root.value("channel_id", 0))));
          std::lock_guard<std::mutex> wlock(part->write_mu);
          (void)WriteJson(part->stream, {{"v", 1}, {"ok", true}, {"op", "unsubscribe"}});
        } else if (op == "detach") {
          {
            std::lock_guard<std::mutex> wlock(part->write_mu);
            (void)WriteJson(part->stream, {{"v", 1}, {"ok", true}, {"op", "detach"}});
          }
          break;
        }
        continue;
      }

      auto frame = DecodeMediaDataFrame(*body);
      if (!frame) {
        break;
      }
      part->bytes_up += static_cast<int64_t>(body->size());
      session->bytes_total += static_cast<int64_t>(body->size());
      Fanout(session, part->peer_id, *frame, *body);
    }

    std::lock_guard<std::mutex> lock(mu);
    session->participants.erase(std::remove_if(session->participants.begin(), session->participants.end(),
                                               [&](const std::shared_ptr<HostParticipant>& p) {
                                                 return p.get() == part.get();
                                               }),
                                session->participants.end());
    part->stream->close([](auto&&) {});
  }

  void HandleInbound(libp2p::StreamAndProtocol stream_and_protocol) {
    auto stream = std::move(stream_and_protocol.stream);
    auto self = shared_from_this();
    std::thread([self, stream = std::move(stream)]() mutable {
      self->HandleInboundBody(std::move(stream));
    }).detach();
  }

  void HandleInboundBody(std::shared_ptr<Stream> stream) {
      std::string remote;
      if (auto peer = stream->remotePeerId()) {
        remote = peer.value().toBase58();
      }

      MediaRelayAdmissionPolicy policy;
      {
        std::lock_guard<std::mutex> lock(mu);
        policy = admission;
      }

      const bool admitted =
          !policy.prefer_contacts_only || policy.contact_peer_ids.empty() ||
          (!remote.empty() && policy.contact_peer_ids.count(remote) > 0);
      if (!admitted) {
        (void)WriteJson(stream, {{"v", 1}, {"ok", false}, {"error", "prefer contacts: stranger refused"}});
        stream->close([](auto&&) {});
        return;
      }

      // Control handshake: quote → accept → attach (may be multi-message)
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

          auto part = std::make_shared<HostParticipant>();
          part->peer_id = remote;
          part->stream = stream;

          {
            std::lock_guard<std::mutex> lock(mu);
            auto it = sessions_by_call.find(call_id);
            if (it != sessions_by_call.end()) {
              session = it->second;
            } else {
              session = std::make_shared<HostSession>();
              session->call_id = call_id;
              session->session_token = token;
              PendingQuote pending;
              auto qit = quotes_by_id.find(accepted_quote_id);
              if (!accepted_quote_id.empty()) {
                // budgets from last accept path stored on session below
              }
              (void)qit;
              session->b_up_bps = OrDefault(budget.max_session_up_bps, kDefaultSessionUpBps);
              session->b_down_bps = OrDefault(budget.max_session_down_bps, kDefaultSessionDownBps);
              session->ceiling_bytes = kDefaultCeilingBytes;
              sessions_by_call[call_id] = session;
              sessions_by_token[token] = session;
            }
            part->a_up_bps = OrDefault(budget.default_per_user_up_bps, kDefaultUserUpBps);
            part->a_down_bps = OrDefault(budget.default_per_user_down_bps, kDefaultUserDownBps);
            session->participants.push_back(part);
          }

          (void)WriteJson(stream, {{"v", 1}, {"ok", true}, {"op", "attach"}});
          HandleParticipantLoop(session, part);
          return;
        } else {
          (void)WriteJson(stream, {{"v", 1}, {"ok", false}, {"error", "unsupported op"}});
          stream->close([](auto&&) {});
          return;
        }
      }
  }

  void StartClientReader(const std::shared_ptr<Impl>& self) {
    if (client_reader_running.exchange(true)) {
      return;
    }
    std::thread([self]() {
      while (self->client_reader_running.load()) {
        std::shared_ptr<Stream> stream;
        std::function<void(MediaDataFrame)> cb;
        {
          std::lock_guard<std::mutex> lock(self->mu);
          stream = self->client_stream;
          cb = self->client_on_frame;
        }
        if (!stream) {
          break;
        }
        auto body = ReadExactFrame(stream);
        if (!body) {
          break;
        }
        if (body->empty()) {
          break;
        }
        if ((*body)[0] == '{') {
          continue; // control ack noise
        }
        auto frame = DecodeMediaDataFrame(*body);
        if (!frame) {
          break;
        }
        if (cb) {
          cb(*frame);
        }
      }
      self->client_reader_running = false;
    }).detach();
  }
};

MediaRelayService::MediaRelayService(Libp2pHost& host, PeerSessionManager& sessions)
    : impl_(std::make_shared<Impl>()), host_(host), sessions_(sessions) {
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

  sessions_.OpenStream(hop_peer_key, {ProtocolName{kMediaRelayProtocolId}},
                       [req = std::move(req), result_promise](libp2p::StreamAndProtocolOrError stream_res) {
                         std::thread([req, result_promise, stream_res = std::move(stream_res)]() mutable {
                           if (!stream_res) {
                             result_promise->set_value(Error("media-relay stream open failed"));
                             return;
                           }
                           auto stream = std::move(stream_res.value().stream);
                           if (!WriteJson(stream, req)) {
                             result_promise->set_value(Error("Failed to send quote"));
                             stream->close([](auto&&) {});
                             return;
                           }
                           auto root = ReadJson(stream);
                           stream->close([](auto&&) {});
                           if (!root) {
                             result_promise->set_value(root.error());
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
                           result_promise->set_value(q);
                         }).detach();
                       });

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  if (result_future.wait_for(std::chrono::milliseconds(wait_ms)) != std::future_status::ready) {
    return Error("media-relay quote timed out");
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
  auto impl = impl_;

  sessions_.OpenStream(
      hop_peer_key, {ProtocolName{kMediaRelayProtocolId}},
      [impl, quote_id, call_id, auth_stub, on_frame = std::move(on_frame), result_promise](
          libp2p::StreamAndProtocolOrError stream_res) mutable {
        std::thread([impl, quote_id, call_id, auth_stub, on_frame = std::move(on_frame), result_promise,
                     stream_res = std::move(stream_res)]() mutable {
          if (!stream_res) {
            result_promise->set_value(Error("media-relay stream open failed"));
            return;
          }
          auto stream = std::move(stream_res.value().stream);
          if (!WriteJson(stream, {{"v", 1}, {"op", "accept"}, {"quote_id", quote_id}})) {
            result_promise->set_value(Error("Failed to send accept"));
            stream->close([](auto&&) {});
            return;
          }
          auto accept_root = ReadJson(stream);
          if (!accept_root || !accept_root->value("ok", false)) {
            result_promise->set_value(Error(accept_root ? accept_root->value("error", "accept failed")
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
            result_promise->set_value(Error("Failed to send attach"));
            stream->close([](auto&&) {});
            return;
          }
          auto attach_root = ReadJson(stream);
          if (!attach_root || !attach_root->value("ok", false)) {
            result_promise->set_value(Error(attach_root ? attach_root->value("error", "attach failed")
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
          impl->StartClientReader(impl);

          MediaRelayAttachResult out;
          out.ok = true;
          out.session_token = token;
          result_promise->set_value(out);
        }).detach();
      });

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  if (result_future.wait_for(std::chrono::milliseconds(wait_ms)) != std::future_status::ready) {
    return Error("media-relay attach timed out");
  }
  return result_future.get();
}

Roe<void> MediaRelayService::Subscribe(uint32_t stream_id, uint16_t channel_id) {
  std::shared_ptr<Stream> stream;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    stream = impl_->client_stream;
  }
  if (!stream) {
    return Error("not attached");
  }
  std::lock_guard<std::mutex> wlock(impl_->client_write_mu);
  return WriteJson(stream, {{"v", 1}, {"op", "subscribe"}, {"stream_id", stream_id}, {"channel_id", channel_id}});
}

Roe<void> MediaRelayService::Unsubscribe(uint32_t stream_id, uint16_t channel_id) {
  std::shared_ptr<Stream> stream;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
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
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    stream = impl_->client_stream;
  }
  if (!stream) {
    return Error("not attached");
  }
  std::lock_guard<std::mutex> wlock(impl_->client_write_mu);
  return WriteExactBody(stream, EncodeMediaDataFrame(frame));
}

void MediaRelayService::Detach() {
  std::shared_ptr<Stream> stream;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    stream = impl_->client_stream;
    impl_->client_stream.reset();
    impl_->client_session_token.clear();
    impl_->client_on_frame = nullptr;
  }
  impl_->client_reader_running = false;
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
  return impl_->client_stream != nullptr;
}

} // namespace pbr
