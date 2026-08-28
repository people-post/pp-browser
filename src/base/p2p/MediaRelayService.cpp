#include "base/p2p/MediaRelayRuntime.h"

#include "base/p2p/SettledWait.h"
#include "common/PbrCompat.h"

namespace pbr {

MediaRelayService::MediaRelayService(Libp2pHost& host, PeerSessionManager& sessions)
    : runtime_(std::make_shared<MediaRelayRuntime>()), host_(host), sessions_(sessions) {
  runtime_->host = &host_;
  runtime_->sessions = &sessions_;
}

MediaRelayService::~MediaRelayService() {
  Stop();
}

void MediaRelayService::Start() {
  if (started_ || !host_.IsRunning()) {
    return;
  }
  started_ = true;
  auto runtime = runtime_;
  host_.GetHost().setProtocolHandler({ProtocolName{kMediaRelayProtocolId}},
                                     [runtime](libp2p::StreamAndProtocol stream) {
                                       runtime->HandleInbound(std::move(stream));
                                     });
}

void MediaRelayService::Stop() {
  started_ = false;
  Detach();
  if (runtime_) {
    runtime_->ShutdownHostSessions();
  }
}

MediaRelayRuntimeStats MediaRelayService::RuntimeStats() const {
  MediaRelayRuntimeStats out;
  if (!started_ || !runtime_) {
    return out;
  }
  std::lock_guard<std::mutex> lock(runtime_->mu);
  for (const auto& [_, session] : runtime_->sessions_by_call) {
    if (!session || session->participants.empty()) {
      continue;
    }
    ++out.active_sessions;
    out.active_participants += session->participants.size();
  }
  return out;
}

void MediaRelayService::SetBudget(const MediaRelayBudgetConfig& budget) {
  std::lock_guard<std::mutex> lock(runtime_->mu);
  runtime_->budget = budget;
}

void MediaRelayService::SetPricing(const RelayPricingConfig& pricing) {
  std::lock_guard<std::mutex> lock(runtime_->mu);
  runtime_->pricing = pricing;
}

void MediaRelayService::SetAdmissionPolicy(MediaRelayAdmissionPolicy policy) {
  std::lock_guard<std::mutex> lock(runtime_->mu);
  runtime_->admission = std::move(policy);
}

Roe<MediaRelayQuote> MediaRelayService::RequestQuote(const std::string& hop_peer_key,
                                                     const MediaRelayQuoteRequest& request,
                                                     int timeout_ms) {
  if (!host_.IsRunning()) {
    return Error("media-relay host not running");
  }
  if (!sessions_.IsReachableForProtocol(hop_peer_key, kMediaRelayProtocolId)) {
    return Error("hop peer endpoint not registered");
  }

  Object req;
  req.set("v", int64_t{1});
  req.set("op", "quote");
  req.set("call_id", request.call_id);
  req.set("participants", int64_t{request.participants});
  req.set("want_up_bps", request.want_up_bps);
  req.set("want_down_bps", request.want_down_bps);

  SettledWait<MediaRelayQuote> wait;
  const bool circuit_backed = sessions_.IsCircuitBacked(hop_peer_key, kMediaRelayProtocolId);
  auto runtime = runtime_;

  sessions_.OpenStream(hop_peer_key, {ProtocolName{kMediaRelayProtocolId}},
                       [req = std::move(req), wait, circuit_backed, runtime, &host = host_](
                           libp2p::StreamAndProtocolOrError stream_res) {
                         PostLibp2pWorker(host, WorkerLane::Normal,
                                          [runtime, req = std::move(req), wait, circuit_backed,
                                           stream_res = std::move(stream_res)]() mutable {
                                            runtime->RunQuoteExchange(std::move(req), circuit_backed,
                                                                      std::move(stream_res), wait);
                                          });
                       });

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  return wait.Wait(std::chrono::milliseconds(wait_ms),
                   Error(std::string("media-relay quote timed out (hop=") + hop_peer_key + ")"));
}



void MediaRelayService::StartClientFrameReader() {
  if (!runtime_) {
    return;
  }
  runtime_->StartClientDuplex(runtime_);
}

void MediaRelayService::SetClientTransportLostHandler(std::function<void()> handler) {
  if (!runtime_) {
    return;
  }
  std::lock_guard<std::mutex> lock(runtime_->mu);
  runtime_->client_transport_lost_handler = std::move(handler);
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
    std::lock_guard<std::mutex> lock(runtime_->mu);
    if (runtime_->local_hop_part && runtime_->local_hop_session &&
        runtime_->local_hop_session->call_id == call_id && runtime_->local_hop_peer_id == *local_pid) {
      runtime_->local_hop_part->local_on_frame = std::move(on_frame);
      MediaRelayAttachResult out;
      out.ok = true;
      out.session_token = runtime_->local_hop_session->session_token;
      logging::getLogger("MediaRelayService").info
          << "AttachAsLocalHop refresh call=" << call_id;
      return out;
    }
  }

  Detach();

  auto part = std::make_shared<HostParticipant>();
  part->peer_id = *local_pid;
  part->local_on_frame = std::move(on_frame);
  part->a_up_bps = OrDefault(runtime_->budget.default_per_user_up_bps, kDefaultUserUpBps);
  part->a_down_bps = OrDefault(runtime_->budget.default_per_user_down_bps, kDefaultUserDownBps);
  MediaRelayRuntime::ConfigureParticipantLimiters(*part);

  std::shared_ptr<HostSession> session;
  std::string token;
  {
    std::lock_guard<std::mutex> lock(runtime_->mu);
    auto it = runtime_->sessions_by_call.find(call_id);
    if (it != runtime_->sessions_by_call.end()) {
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
      if (!runtime_->CanOpenNewHostSessionLocked()) {
        return Error("host session limit");
      }
      session = std::make_shared<HostSession>();
      session->call_id = call_id;
      session->session_token = MakeId("s");
      session->b_up_bps = OrDefault(runtime_->budget.max_session_up_bps, kDefaultSessionUpBps);
      session->b_down_bps = OrDefault(runtime_->budget.max_session_down_bps, kDefaultSessionDownBps);
      session->ceiling_bytes = kDefaultCeilingBytes;
      MediaRelayRuntime::ConfigureSessionLimiters(*session);
      token = session->session_token;
      runtime_->sessions_by_call[call_id] = session;
      runtime_->sessions_by_token[token] = session;
    }
    // Replace a prior local hop participant for this peer (re-attach).
    session->participants.erase(
        std::remove_if(session->participants.begin(), session->participants.end(),
                       [&](const std::shared_ptr<HostParticipant>& p) {
                         return p && p->peer_id == part->peer_id && p->local_on_frame;
                       }),
        session->participants.end());
    session->participants.push_back(part);
    runtime_->local_hop_part = part;
    runtime_->local_hop_session = session;
    runtime_->local_hop_peer_id = part->peer_id;
    runtime_->client_session_token = token;
  }

  MediaRelayAttachResult out;
  out.ok = true;
  out.session_token = token;
  return out;
}

Roe<void> MediaRelayService::Subscribe(uint32_t stream_id, uint16_t channel_id) {
  const uint64_t key = SubKey(stream_id, channel_id);
  {
    std::lock_guard<std::mutex> lock(runtime_->mu);
    if (runtime_->local_hop_part) {
      if (!runtime_->local_hop_part->subscriptions.insert(key).second) {
        return {}; // already subscribed — avoid log/control spam
      }
      logging::getLogger("MediaRelayService").info
          << "local-hop subscribe stream=" << stream_id << " ch=" << channel_id
          << " call=" << (runtime_->local_hop_session ? runtime_->local_hop_session->call_id : "");
      return {};
    }
    if (runtime_->client_subscriptions.count(key) != 0) {
      return {}; // already sent on this attach
    }
    if (!runtime_->client_duplex) {
      return Error("not attached");
    }
    runtime_->client_subscriptions.insert(key);
  }
  logging::getLogger("MediaRelayService").info
      << "client subscribe stream=" << stream_id << " ch=" << channel_id;
  Object sub;
  sub.set("v", int64_t{1});
  sub.set("op", "subscribe");
  sub.setJsonUInt("stream_id", stream_id);
  sub.setJsonUInt("channel_id", channel_id);
  const std::string json = DumpJson(sub);
  if (!runtime_->EnqueueClientBody(std::vector<uint8_t>(json.begin(), json.end()))) {
    std::lock_guard<std::mutex> lock(runtime_->mu);
    runtime_->client_subscriptions.erase(key);
    return Error("not attached");
  }
  return {};
}

Roe<void> MediaRelayService::Unsubscribe(uint32_t stream_id, uint16_t channel_id) {
  const uint64_t key = SubKey(stream_id, channel_id);
  {
    std::lock_guard<std::mutex> lock(runtime_->mu);
    if (runtime_->local_hop_part) {
      runtime_->local_hop_part->subscriptions.erase(key);
      return {};
    }
    runtime_->client_subscriptions.erase(key);
    if (!runtime_->client_duplex) {
      return Error("not attached");
    }
  }
  Object unsub;
  unsub.set("v", int64_t{1});
  unsub.set("op", "unsubscribe");
  unsub.setJsonUInt("stream_id", stream_id);
  unsub.setJsonUInt("channel_id", channel_id);
  const std::string json = DumpJson(unsub);
  if (!runtime_->EnqueueClientBody(std::vector<uint8_t>(json.begin(), json.end()))) {
    return Error("not attached");
  }
  return {};
}

Roe<void> MediaRelayService::SendFrame(const MediaDataFrame& frame) {
  std::shared_ptr<Stream> stream;
  std::shared_ptr<HostSession> session;
  std::string from_peer;
  {
    std::lock_guard<std::mutex> lock(runtime_->mu);
    stream = runtime_->client_stream;
    if (runtime_->local_hop_part) {
      session = runtime_->local_hop_session;
      from_peer = runtime_->local_hop_peer_id;
    }
  }
  if (session) {
    const std::vector<uint8_t> body = EncodeMediaDataFrame(frame);
    const int64_t nbytes = static_cast<int64_t>(body.size());
    const int64_t now_ms = util::NowUnixMs();
    {
      std::lock_guard<std::mutex> lock(runtime_->mu);
      if (runtime_->local_hop_part) {
        if (!runtime_->local_hop_part->up_limiter.TryConsume(nbytes, now_ms) ||
            !session->session_up_limiter.TryConsume(nbytes, now_ms)) {
          ++runtime_->local_hop_part->drops_rate;
          ++session->drops_total;
          ++session->drops_rate;
          return {}; // drop excess uplink; keep session
        }
        if (session->ceiling_bytes > 0 && session->bytes_total > session->ceiling_bytes) {
          ++session->drops_total;
          ++session->drops_ceiling;
          return {};
        }
        runtime_->local_hop_part->bytes_up += nbytes;
        session->bytes_total += nbytes;
        session->bytes_up_window += nbytes;
      }
    }
    runtime_->Fanout(session, from_peer, frame, body);
    return {};
  }
  if (!stream) {
    return Error("not attached");
  }
  if (!runtime_->EnqueueClientBody(EncodeMediaDataFrame(frame),
                                   frame.channel_type == MediaChannelType::LatestLossy)) {
    return Error("not attached");
  }
  return {};
}

Roe<void> MediaRelayService::EnqueueRawClientBodyForTest(std::vector<uint8_t> body) {
  if (!runtime_ || !runtime_->EnqueueClientBody(std::move(body))) {
    return Error("not attached");
  }
  return {};
}

void MediaRelayService::Detach() {
  std::shared_ptr<Stream> stream;
  std::shared_ptr<HostParticipant> local;
  std::shared_ptr<HostSession> local_session;
  {
    std::lock_guard<std::mutex> lock(runtime_->mu);
    const MediaRelayClientPhase prev = runtime_->ClientPhase();
    if (prev != MediaRelayClientPhase::Idle) {
      runtime_->SetClientPhaseLocked(MediaRelayClientPhase::Detaching,
                                  MediaRelayClientEvent::DetachRequested, runtime_->client_call_id);
    }
    // Unblock AcceptAndAttach waiters (Leave / SoftMigrate / supersede).
    runtime_->CompleteClientAttachLocked(Error("media-relay attach aborted"));
    runtime_->StopClientDuplexLocked();
    stream = runtime_->client_stream;
    runtime_->client_stream.reset();
    runtime_->client_session_token.clear();
    runtime_->client_on_frame = nullptr;
    runtime_->client_subscriptions.clear();
    local = runtime_->local_hop_part;
    local_session = runtime_->local_hop_session;
    runtime_->local_hop_part.reset();
    runtime_->local_hop_session.reset();
    runtime_->local_hop_peer_id.clear();
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
      runtime_->SetClientPhaseLocked(MediaRelayClientPhase::Idle, MediaRelayClientEvent::DetachRequested);
    }
  }
  runtime_->client_reader_epoch.fetch_add(1, std::memory_order_acq_rel);
  if (stream) {
    // Close so in-flight async read/write complete and Leave can join capture.
    stream->close([](auto&&) {});
  }
}

bool MediaRelayService::IsAttached() const {
  std::lock_guard<std::mutex> lock(runtime_->mu);
  return runtime_->client_stream != nullptr || runtime_->local_hop_part != nullptr;
}

MediaRelayClientPhase MediaRelayService::ClientPhase() const {
  return runtime_->ClientPhase();
}

bool MediaRelayService::IsLocalHopAttached() const {
  std::lock_guard<std::mutex> lock(runtime_->mu);
  return runtime_->local_hop_part != nullptr;
}

double MediaRelayService::PathPressure() const {
  return HealthSnapshot().path_pressure;
}

CallHopHealth MediaRelayService::HealthSnapshot() const {
  CallHopHealth h;
  std::lock_guard<std::mutex> lock(runtime_->mu);
  h.attached = runtime_->local_hop_part != nullptr || runtime_->client_stream != nullptr;
  const HostSession* session = nullptr;
  if (runtime_->local_hop_session) {
    session = runtime_->local_hop_session.get();
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

void MediaRelayRuntime::RunQuoteExchange(Object req, bool circuit_backed,
                                         libp2p::StreamAndProtocolOrError stream_res,
                                         const SettledWait<MediaRelayQuote>& wait) {
  if (!stream_res) {
    const auto& ec = stream_res.error();
    wait.Finish(Error(std::string("media-relay stream open failed: ") + ec.message()));
    return;
  }
  auto stream = std::move(stream_res.value().stream);
  if (!WriteJson(stream, req)) {
    wait.Finish(Error("Failed to send quote"));
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
    wait.Finish(root.error());
    return;
  }
  auto readDouble = [](const Object& o, const char* key, double def) {
    if (auto d = o.getIf<double>(key)) {
      return *d;
    }
    if (auto i = o.getIf<int64_t>(key)) {
      return static_cast<double>(*i);
    }
    return def;
  };
  MediaRelayQuote q;
  q.ok = root->getIf<bool>("ok").value_or(false);
  q.error = root->getString("error").value_or("");
  q.quote_id = root->getString("quote_id").value_or("");
  q.a_up_bps = root->getIf<int64_t>("A_up").value_or(0);
  q.a_down_bps = root->getIf<int64_t>("A_down").value_or(0);
  q.b_up_bps = root->getIf<int64_t>("B_up").value_or(0);
  q.b_down_bps = root->getIf<int64_t>("B_down").value_or(0);
  q.pricing_mode = root->getString("mode").value_or("volunteer");
  q.rate = readDouble(*root, "rate", 0.0);
  q.ceiling_bytes = root->getIf<int64_t>("ceiling_bytes").value_or(0);
  q.ceiling_amount = readDouble(*root, "ceiling_amount", 0.0);
  wait.Finish(q);
}

} // namespace pbr
