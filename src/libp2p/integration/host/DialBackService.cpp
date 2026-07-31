#include "libp2p/integration/host/DialBackService.h"

#include <libp2p/basic/read.hpp>
#include <libp2p/basic/write.hpp>
#include <libp2p/connection/stream.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace pbr {

namespace {

using libp2p::Bytes;
using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

constexpr size_t kMaxDialBackJsonBytes = 64 * 1024;

Roe<std::vector<uint8_t>> EncodeJsonFrame(const std::string& json_utf8) {
  if (json_utf8.size() > kMaxDialBackJsonBytes) {
    return Error("dial-back frame too large");
  }
  std::vector<uint8_t> frame(8 + json_utf8.size());
  uint64_t len = json_utf8.size();
  for (int i = 7; i >= 0; --i) {
    frame[static_cast<size_t>(i)] = static_cast<uint8_t>(len & 0xff);
    len >>= 8;
  }
  std::copy(json_utf8.begin(), json_utf8.end(), frame.begin() + 8);
  return frame;
}

Roe<std::string> DecodeJsonFrame(const std::vector<uint8_t>& frame_bytes) {
  if (frame_bytes.size() < 8) {
    return Error("dial-back frame truncated");
  }
  uint64_t payload_len = 0;
  for (size_t i = 0; i < 8; ++i) {
    payload_len = (payload_len << 8) | frame_bytes[i];
  }
  if (payload_len > kMaxDialBackJsonBytes || frame_bytes.size() != 8 + payload_len) {
    return Error("dial-back frame length mismatch");
  }
  return std::string(frame_bytes.begin() + 8, frame_bytes.end());
}

Roe<std::vector<uint8_t>> ReadExactFrame(const std::shared_ptr<Stream>& stream) {
  Bytes header(8);
  std::promise<outcome::result<void>> header_promise;
  auto header_future = header_promise.get_future();
  libp2p::read(stream, header, [&](outcome::result<void> result) { header_promise.set_value(result); });
  if (!header_future.get()) {
    return Error("Failed to read dial-back frame header");
  }

  std::vector<uint8_t> frame(header.begin(), header.end());
  uint64_t payload_len = 0;
  for (size_t i = 0; i < 8; ++i) {
    payload_len = (payload_len << 8) | frame[i];
  }
  if (payload_len > kMaxDialBackJsonBytes) {
    return Error("dial-back frame too large");
  }

  Bytes payload(payload_len);
  std::promise<outcome::result<void>> body_promise;
  auto body_future = body_promise.get_future();
  libp2p::read(stream, payload, [&](outcome::result<void> result) { body_promise.set_value(result); });
  if (!body_future.get()) {
    return Error("Failed to read dial-back frame body");
  }

  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

Roe<void> WriteExactFrame(const std::shared_ptr<Stream>& stream, const std::vector<uint8_t>& frame) {
  std::promise<outcome::result<void>> write_promise;
  auto write_future = write_promise.get_future();
  libp2p::write(stream, libp2p::Bytes(frame), [&](outcome::result<void> result) { write_promise.set_value(result); });
  if (!write_future.get()) {
    return Error("Failed to write dial-back frame");
  }
  return {};
}

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

struct DialBackService::Impl {
  std::mutex handler_mutex;
  PeerSessionManager* sessions = nullptr;

  void HandleStream(libp2p::StreamAndProtocol stream_and_protocol) {
    // Protocol handlers run on the host io thread — do not block it (dial would deadlock).
    auto stream = std::move(stream_and_protocol.stream);
    std::thread([this, stream = std::move(stream)]() mutable {
      auto frame = ReadExactFrame(stream);
      if (!frame) {
        stream->close([](auto&&) {});
        return;
      }
      auto json_utf8 = DecodeJsonFrame(*frame);
      if (!json_utf8) {
        stream->close([](auto&&) {});
        return;
      }
      nlohmann::json root = nlohmann::json::parse(*json_utf8, nullptr, false);
      DialBackProbeResult result;
      if (root.is_discarded() || !root.is_object()) {
        result.error = "invalid dial-back json";
      } else {
        const std::string op = root.value("op", "");
        if (op != "probe") {
          result.error = "unsupported op";
        } else if (!sessions) {
          result.error = "dial-back service not ready";
        } else {
          std::vector<std::string> targets;
          if (root.contains("target_multiaddrs") && root["target_multiaddrs"].is_array()) {
            for (const auto& item : root["target_multiaddrs"]) {
              if (item.is_string()) {
                targets.push_back(item.get<std::string>());
              }
            }
          }
          const int timeout_ms = root.value("timeout_ms", 8000);
          result = DialTargets(*sessions, targets, timeout_ms);
        }
      }

      nlohmann::json response = {
          {"v", 1},
          {"ok", result.ok},
          {"dialed", result.dialed},
          {"error", result.error},
      };
      if (auto encoded = EncodeJsonFrame(response.dump())) {
        (void)WriteExactFrame(stream, *encoded);
      }
      stream->close([](auto&&) {});
    }).detach();
  }
};

DialBackService::DialBackService(Libp2pHost& host, PeerSessionManager& sessions)
    : impl_(std::make_unique<Impl>()), host_(host), sessions_(sessions) {
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
                                     [impl = impl_.get()](libp2p::StreamAndProtocol stream) {
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

  nlohmann::json request = {
      {"v", 1},
      {"op", "probe"},
      {"target_multiaddrs", target_multiaddrs},
      {"timeout_ms", timeout_ms > 0 ? timeout_ms : 8000},
  };
  auto frame = EncodeJsonFrame(request.dump());
  if (!frame) {
    return frame.error();
  }

  std::shared_ptr<std::promise<Roe<DialBackProbeResult>>> result_promise =
      std::make_shared<std::promise<Roe<DialBackProbeResult>>>();
  auto result_future = result_promise->get_future();

  sessions_.OpenStream(seed_peer_key, {ProtocolName{kDialBackProtocolId}},
                       [frame = *frame, result_promise](libp2p::StreamAndProtocolOrError stream_res) {
                         // newStream callbacks run on the host io thread — hop off before blocking I/O.
                         std::thread([frame, result_promise, stream_res = std::move(stream_res)]() mutable {
                           if (!stream_res) {
                             result_promise->set_value(Error("dial-back stream open failed"));
                             return;
                           }
                           auto stream = std::move(stream_res.value().stream);
                           if (!WriteExactFrame(stream, frame)) {
                             result_promise->set_value(Error("Failed to send dial-back probe"));
                             return;
                           }
                           auto response_frame = ReadExactFrame(stream);
                           stream->close([](auto&&) {});
                           if (!response_frame) {
                             result_promise->set_value(Error("Failed to read dial-back response"));
                             return;
                           }
                           auto json_utf8 = DecodeJsonFrame(*response_frame);
                           if (!json_utf8) {
                             result_promise->set_value(json_utf8.error());
                             return;
                           }
                           nlohmann::json root = nlohmann::json::parse(*json_utf8, nullptr, false);
                           if (root.is_discarded() || !root.is_object()) {
                             result_promise->set_value(Error("invalid dial-back response"));
                             return;
                           }
                           DialBackProbeResult parsed;
                           parsed.ok = root.value("ok", false);
                           parsed.dialed = root.value("dialed", "");
                           parsed.error = root.value("error", "");
                           result_promise->set_value(parsed);
                         }).detach();
                       });

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  if (result_future.wait_for(std::chrono::milliseconds(wait_ms)) != std::future_status::ready) {
    return Error("dial-back probe timed out");
  }
  return result_future.get();
}

} // namespace pbr
