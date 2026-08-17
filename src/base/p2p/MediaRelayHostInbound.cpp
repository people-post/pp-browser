#include "base/p2p/MediaRelayRuntime.h"

namespace pbr {

void MediaRelayRuntime::HandleInbound(libp2p::StreamAndProtocol stream_and_protocol) {
    if (!host) {
      return;
    }
    auto stream = std::move(stream_and_protocol.stream);
    auto self = shared_from_this();
    PostLibp2pWorker(*host, WorkerLane::Normal, [self, stream = std::move(stream)]() mutable {
      self->HandleInboundBody(std::move(stream));
    });
  }

void MediaRelayRuntime::HandleInboundBody(std::shared_ptr<Stream> stream) {
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

        // SoftMigrate / guest Detach→reattach: replace a prior remote participant for the
        // same peer before the old duplex FIN cleanup lands (half-open double entry).
        std::vector<std::shared_ptr<HostParticipant>> replaced;
        {
          std::lock_guard<std::mutex> lock(mu);
          auto it = sessions_by_call.find(call_id);
          if (it != sessions_by_call.end()) {
            session = it->second;
            size_t others = 0;
            for (const auto& p : session->participants) {
              if (p && !(p->peer_id == part->peer_id && !p->local_on_frame)) {
                ++others;
              }
            }
            if (others >= MediaRelayService::kMaxParticipantsPerSession) {
              RejectAndCloseAttach(sm, stream, "session participant limit",
                                   MediaRelayAttachEvent::AttachFail);
              return;
            }
            session->participants.erase(
                std::remove_if(session->participants.begin(), session->participants.end(),
                               [&](const std::shared_ptr<HostParticipant>& p) {
                                 if (!p || p->peer_id != part->peer_id || p->local_on_frame) {
                                   return false;
                                 }
                                 replaced.push_back(p);
                                 return true;
                               }),
                session->participants.end());
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
        for (const auto& old : replaced) {
          CleanupParticipant(session, old, "replaced_by_reattach");
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

} // namespace pbr
