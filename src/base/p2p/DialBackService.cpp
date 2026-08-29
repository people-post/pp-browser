#include "base/p2p/DialBackService.h"

#include "base/p2p/Libp2pWorker.h"
#include "base/p2p/SettledWait.h"
#include "base/p2p/StreamJsonFrame.h"
#include "common/ValueJson.h"

#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

DialBackProbeResult DialTargets(PeerSessionManager& sessions,
                                const std::vector<std::string>& targets,
                                int timeout_ms) {
  DialBackProbeResult out;
  if (targets.empty()) {
    out.error = "no target_multiaddrs";
    return out;
  }
  const int timeout = timeout_ms > 0 ? timeout_ms : 8000;
  for (size_t i = 0; i < targets.size(); ++i) {
    const std::string& ma = targets[i];
    if (ma.empty()) {
      continue;
    }
    const std::string key = "dialback:probe:" + std::to_string(i);
    if (auto registered = sessions.RegisterEndpoint(key, ma); !registered) {
      out.error = registered.error().message;
      continue;
    }
    sessions.ClearDialBackoff(key);

    // shared_ptr: wait_for may time out while EnsureConnection is still in flight; a stack
    // promise would be destroyed and the completion callback would UAF (Windows often hits
    // this on slow connect-fail paths such as probing 127.0.0.1:1).
    auto dial_promise = std::make_shared<std::promise<Roe<void>>>();
    auto dial_future = dial_promise->get_future();
    sessions.EnsureConnection(key, [dial_promise](Roe<void> result) {
      try {
        dial_promise->set_value(std::move(result));
      } catch (const std::future_error&) {
        // Already satisfied (should not happen) or abandoned after move.
      }
    });

    if (dial_future.wait_for(std::chrono::milliseconds(timeout)) != std::future_status::ready) {
      out.error = "dial-back timed out";
      out.dialed = ma;
      continue;
    }
    auto dialed = dial_future.get();
    if (dialed) {
      out.ok = true;
      out.dialed = ma;
      out.error.clear();
      return out;
    }
    out.error = dialed.error().message;
    out.dialed = ma;
  }
  return out;
}

} // namespace

struct DialBackService::Impl : std::enable_shared_from_this<Impl> {
  std::mutex handler_mutex;
  Libp2pHost* host = nullptr;
  PeerSessionManager* sessions = nullptr;

  void HandleStream(libp2p::StreamAndProtocol stream_and_protocol) {
    // Protocol handlers run on the host io thread — hop to control worker before blocking I/O.
    if (!host) {
      return;
    }
    auto stream = std::move(stream_and_protocol.stream);
    auto self = shared_from_this();
    PostLibp2pWorker(*host, WorkerLane::Normal, [self, stream = std::move(stream)]() mutable {
      auto json_utf8 = BlockingReadStreamJson(stream);
      if (!json_utf8) {
        stream->close([](auto&&) {});
        return;
      }
      DialBackProbeResult result;
      auto root = TryParseObject(*json_utf8);
      if (!root) {
        result.error = "invalid dial-back json";
      } else {
        const std::string op = root->getString("op").value_or("");
        if (op != "probe") {
          result.error = "unsupported op";
        } else if (!self->sessions) {
          result.error = "dial-back service not ready";
        } else {
          std::vector<std::string> targets;
          if (const Array* addrs = root->getArray("target_multiaddrs")) {
            for (const auto& item : addrs->elements) {
              if (auto s = asString(item)) {
                targets.push_back(*s);
              }
            }
          }
          const int timeout_ms =
              static_cast<int>(root->getNonNegInt("timeout_ms").value_or(8000));
          result = DialTargets(*self->sessions, targets, timeout_ms);
        }
      }

      Object response;
      response.set("v", int64_t{1});
      response.set("ok", result.ok);
      response.set("dialed", result.dialed);
      response.set("error", result.error);
      (void)BlockingWriteStreamJson(stream, DumpJson(response));
      stream->close([](auto&&) {});
    });
  }
};

DialBackService::DialBackService(Libp2pHost& host, PeerSessionManager& sessions)
    : impl_(std::make_shared<Impl>()), host_(host), sessions_(sessions) {
  impl_->host = &host_;
  impl_->sessions = &sessions_;
}

DialBackService::~DialBackService() {
  Stop();
}

void DialBackService::Start() {
  if (started_ || !host_.IsRunning()) {
    return;
  }
  started_ = true;
  host_.GetHost().setProtocolHandler({ProtocolName{kDialBackProtocolId}},
                                     [impl = impl_](libp2p::StreamAndProtocol stream) {
                                       impl->HandleStream(std::move(stream));
                                     });
}

void DialBackService::Stop() {
  started_ = false;
}

Roe<DialBackProbeResult> DialBackService::Probe(const std::string& seed_peer_key,
                                                const std::vector<std::string>& target_multiaddrs,
                                                int timeout_ms) {
  if (!started_ || !host_.IsRunning()) {
    return Error("dial-back service not started");
  }
  if (!sessions_.IsDialable(seed_peer_key)) {
    return Error("seed peer endpoint not registered");
  }
  if (target_multiaddrs.empty()) {
    return Error("no target_multiaddrs");
  }

  Object request;
  request.set("v", int64_t{1});
  request.set("op", "probe");
  std::vector<Value> addrs;
  addrs.reserve(target_multiaddrs.size());
  for (const auto& ma : target_multiaddrs) {
    addrs.emplace_back(ma);
  }
  request.set("target_multiaddrs", ArrayValue(std::move(addrs)));
  request.set("timeout_ms", int64_t{timeout_ms > 0 ? timeout_ms : 8000});

  SettledWait<DialBackProbeResult> wait;

  sessions_.OpenStream(seed_peer_key, {ProtocolName{kDialBackProtocolId}},
                       [&host = host_, request = DumpJson(request), wait](
                           libp2p::StreamAndProtocolOrError stream_res) {
                         // newStream callbacks run on the host io thread — hop off before blocking I/O.
                         PostLibp2pWorker(host, WorkerLane::Normal,
                                          [request, wait,
                                           stream_res = std::move(stream_res)]() mutable {
                                            if (!stream_res) {
                                              wait.Finish(Error("dial-back stream open failed"));
                                              return;
                                            }
                                            auto stream = std::move(stream_res.value().stream);
                                            if (!BlockingWriteStreamJson(stream, request)) {
                                              wait.Finish(Error("Failed to send dial-back probe"));
                                              return;
                                            }
                                            auto response_json = BlockingReadStreamJson(stream);
                                            stream->close([](auto&&) {});
                                            if (!response_json) {
                                              wait.Finish(Error("Failed to read dial-back response"));
                                              return;
                                            }
                                            auto root = TryParseObject(*response_json);
                                            if (!root) {
                                              wait.Finish(Error("invalid dial-back response"));
                                              return;
                                            }
                                            DialBackProbeResult parsed;
                                            parsed.ok = root->getIf<bool>("ok").value_or(false);
                                            parsed.dialed = root->getString("dialed").value_or("");
                                            parsed.error = root->getString("error").value_or("");
                                            wait.Finish(parsed);
                                          });
                       });

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  return wait.Wait(std::chrono::milliseconds(wait_ms), Error("dial-back probe timed out"));
}

} // namespace pbr
