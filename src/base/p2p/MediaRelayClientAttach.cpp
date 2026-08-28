#include "base/p2p/MediaRelayRuntime.h"
#include "common/PbrCompat.h"

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
  auto runtime_keepalive = runtime_;

  {
    std::lock_guard<std::mutex> lock(runtime_keepalive->mu);
    runtime_keepalive->client_attach_settled = settled;
    runtime_keepalive->client_attach_promise = result_promise;
    (void)runtime_keepalive->ApplyClientLocked(MediaRelayClientEvent::AttachRequested, call_id);
  }

  std::weak_ptr<MediaRelayRuntime> weak_runtime = runtime_keepalive;
  sessions_.OpenStream(
      hop_peer_key, {ProtocolName{kMediaRelayProtocolId}},
      [weak_runtime, quote_id, call_id, auth_stub, on_frame = std::move(on_frame), settled,
       &host = host_](libp2p::StreamAndProtocolOrError stream_res) mutable {
        auto runtime = weak_runtime.lock();
        if (!runtime) {
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
                         [weak_runtime, quote_id, call_id, auth_stub, on_frame = std::move(on_frame),
                          settled, stream_res = std::move(stream_res)]() mutable {
                           auto locked_runtime = weak_runtime.lock();
                           if (!locked_runtime) {
                             if (stream_res) {
                               stream_res.value().stream->close([](auto&&) {});
                             }
                             return;
                           }
                           locked_runtime->RunClientAttachOnWorker(quote_id, call_id, auth_stub,
                                                            std::move(on_frame), settled,
                                                            std::move(stream_res));
                         });
      });

  // Slice the wait so Detach can complete the promise without blocking Leave for the full timeout.
  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  for (;;) {
    const auto status = result_future.wait_for(std::chrono::milliseconds(50));
    if (status == std::future_status::ready) {
      std::lock_guard<std::mutex> lock(runtime_keepalive->mu);
      if (runtime_keepalive->client_attach_settled == settled) {
        runtime_keepalive->client_attach_settled.reset();
        runtime_keepalive->client_attach_promise.reset();
      }
      return result_future.get();
    }
    {
      std::lock_guard<std::mutex> lock(runtime_keepalive->mu);
      if (runtime_keepalive->ClientPhase() == MediaRelayClientPhase::Attached && runtime_keepalive->client_stream) {
        settled->store(true, std::memory_order_release);
        if (runtime_keepalive->client_attach_settled == settled) {
          runtime_keepalive->client_attach_settled.reset();
          runtime_keepalive->client_attach_promise.reset();
        }
        MediaRelayAttachResult out;
        out.ok = true;
        out.session_token = runtime_keepalive->client_session_token;
        return out;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      settled->exchange(true);
      {
        std::lock_guard<std::mutex> lock(runtime_keepalive->mu);
        if (runtime_keepalive->client_attach_settled == settled) {
          runtime_keepalive->client_attach_settled.reset();
          runtime_keepalive->client_attach_promise.reset();
        }
        if (runtime_keepalive->ClientPhase() == MediaRelayClientPhase::Attached && runtime_keepalive->client_stream) {
          MediaRelayAttachResult out;
          out.ok = true;
          out.session_token = runtime_keepalive->client_session_token;
          return out;
        }
        // Drop any late-installed stream from a racing worker (should be rare after settled check).
        if (runtime_keepalive->ApplyClientLocked(MediaRelayClientEvent::AttachTimeout, call_id)) {
          runtime_keepalive->StopClientDuplexLocked();
          if (runtime_keepalive->client_stream) {
            runtime_keepalive->client_stream->close([](auto&&) {});
            runtime_keepalive->client_stream.reset();
          }
          runtime_keepalive->client_session_token.clear();
          runtime_keepalive->client_on_frame = nullptr;
          runtime_keepalive->client_subscriptions.clear();
        }
      }
      return Error(std::string("media-relay attach timed out (hop=") + hop_peer_key + ")");
    }
  }
}

void MediaRelayRuntime::RunClientAttachOnWorker(
    const std::string& quote_id, const std::string& call_id, const std::string& auth_stub,
    std::function<void(MediaDataFrame)> on_frame, std::shared_ptr<std::atomic<bool>> settled,
    libp2p::StreamAndProtocolOrError stream_res) {
  auto finish = [&](Roe<MediaRelayAttachResult> value, MediaRelayClientEvent ev) {
    std::lock_guard<std::mutex> lock(mu);
    if (settled->load(std::memory_order_acquire)) {
      return;
    }
    if (!value) {
      (void)ApplyClientLocked(ev, call_id);
    }
    CompleteClientAttachLocked(std::move(value));
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
    std::lock_guard<std::mutex> lock(mu);
    if (settled->load(std::memory_order_acquire) ||
        !ApplyClientLocked(MediaRelayClientEvent::OpenStreamOk, call_id)) {
      stream->close([](auto&&) {});
      return;
    }
  }
  Object accept_req;
  accept_req.set("v", int64_t{1});
  accept_req.set("op", "accept");
  accept_req.set("quote_id", quote_id);
  if (!WriteJson(stream, accept_req)) {
    finish(Error("Failed to send accept"), MediaRelayClientEvent::AcceptFail);
    stream->close([](auto&&) {});
    return;
  }
  auto accept_root = ReadJson(stream);
  if (settled->load(std::memory_order_acquire)) {
    stream->close([](auto&&) {});
    return;
  }
  if (!accept_root || !accept_root->getIf<bool>("ok").value_or(false)) {
    finish(Error(accept_root ? accept_root->getString("error").value_or("accept failed")
                             : accept_root.error().message),
           MediaRelayClientEvent::AcceptFail);
    stream->close([](auto&&) {});
    return;
  }
  const std::string token = accept_root->getString("session_token").value_or("");
  {
    std::lock_guard<std::mutex> lock(mu);
    if (settled->load(std::memory_order_acquire) ||
        !ApplyClientLocked(MediaRelayClientEvent::AcceptOk, call_id)) {
      stream->close([](auto&&) {});
      return;
    }
  }
  Object attach_req;
  attach_req.set("v", int64_t{1});
  attach_req.set("op", "attach");
  attach_req.set("session_token", token);
  attach_req.set("call_id", call_id);
  attach_req.set("auth", auth_stub);
  if (!WriteJson(stream, attach_req)) {
    finish(Error("Failed to send attach"), MediaRelayClientEvent::AttachFail);
    stream->close([](auto&&) {});
    return;
  }
  auto attach_root = ReadJson(stream);
  if (settled->load(std::memory_order_acquire)) {
    stream->close([](auto&&) {});
    return;
  }
  if (!attach_root || !attach_root->getIf<bool>("ok").value_or(false)) {
    finish(Error(attach_root ? attach_root->getString("error").value_or("attach failed")
                             : attach_root.error().message),
           MediaRelayClientEvent::AttachFail);
    stream->close([](auto&&) {});
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mu);
    if (settled->load(std::memory_order_acquire) || ClientPhase() == MediaRelayClientPhase::Idle ||
        ClientPhase() == MediaRelayClientPhase::Detaching ||
        !ApplyClientLocked(MediaRelayClientEvent::AttachOk, call_id)) {
      stream->close([](auto&&) {});
      return;
    }
    client_stream = stream;
    client_session_token = token;
    client_on_frame = std::move(on_frame);
    client_subscriptions.clear();
    MediaRelayAttachResult out;
    out.ok = true;
    out.session_token = token;
    CompleteClientAttachLocked(out);
  }
}

} // namespace pbr
