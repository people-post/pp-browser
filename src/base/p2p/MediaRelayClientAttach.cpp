#include "base/p2p/MediaRelayServiceImpl.h"

namespace pbr {

Roe<MediaRelayAttachResult> MediaRelayService::AcceptAndAttach(
    const std::string& hop_peer_key, const std::string& quote_id, const std::string& call_id,
    const std::string& auth_stub, std::function<void(MediaDataFrame)> on_frame, int timeout_ms) {
  if (!host_.IsRunning()) {
    return Error("media-relay host not running");
  }
  if (!sessions_.IsReachableForProtocol(hop_peer_key, kMediaRelayProtocolId)) {
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

  std::weak_ptr<MediaRelayService::Impl> weak_impl = impl;
  sessions_.OpenStream(
      hop_peer_key, {ProtocolName{kMediaRelayProtocolId}},
      [weak_impl, quote_id, call_id, auth_stub, on_frame = std::move(on_frame), settled,
       &host = host_](libp2p::StreamAndProtocolOrError stream_res) mutable {
        auto impl = weak_impl.lock();
        if (!impl) {
          if (stream_res) {
            stream_res.value().stream->close([](auto&&) {});
          }
          return;
        }
        // Detach/timeout may have settled before this cb; do not post work that touches mu.
        if (settled->load(std::memory_order_acquire)) {
          if (stream_res) {
            stream_res.value().stream->close([](auto&&) {});
          }
          return;
        }
        PostLibp2pWorker(host, WorkerLane::Normal,
                         [weak_impl, quote_id, call_id, auth_stub, on_frame = std::move(on_frame), settled,
                          stream_res = std::move(stream_res)]() mutable {
                           auto impl = weak_impl.lock();
                           if (!impl) {
                             if (stream_res) {
                               stream_res.value().stream->close([](auto&&) {});
                             }
                             return;
                           }
                           auto finish = [&](Roe<MediaRelayAttachResult> value,
                                             MediaRelayClientEvent ev) {
                             std::lock_guard<std::mutex> lock(impl->mu);
                             if (settled->load(std::memory_order_acquire)) {
                               return;
                             }
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

} // namespace pbr
