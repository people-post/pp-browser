#include "libp2p/integration/host/MediaRelayServiceImpl.h"

namespace pbr {

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
  if (impl_) {
    impl_->ShutdownHostSessions();
  }
}

MediaRelayRuntimeStats MediaRelayService::RuntimeStats() const {
  MediaRelayRuntimeStats out;
  if (!started_ || !impl_) {
    return out;
  }
  std::lock_guard<std::mutex> lock(impl_->mu);
  for (const auto& [_, session] : impl_->sessions_by_call) {
    if (!session || session->participants.empty()) {
      continue;
    }
    ++out.active_sessions;
    out.active_participants += session->participants.size();
  }
  return out;
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

Roe<void> MediaRelayService::EnqueueRawClientBodyForTest(std::vector<uint8_t> body) {
  if (!impl_ || !impl_->EnqueueClientBody(std::move(body))) {
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
