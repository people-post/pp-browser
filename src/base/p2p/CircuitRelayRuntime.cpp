#include "base/p2p/CircuitRelayRuntime.h"

#include "base/p2p/CircuitBridgeTarget.h"
#include "base/p2p/Libp2pWorker.h"
#include "base/p2p/StreamJsonFrame.h"
#include "base/people/RelayScope.h"
#include "common/ValueJson.h"

#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <algorithm>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

using libp2p::connection::Stream;
using libp2p::peer::ProtocolName;

} // namespace

void CircuitRelayRuntime::AbortInflightLocked() {
  for (auto& entry : inflight_bridges) {
    entry.wait.Finish(Error("circuit-relay aborted"));
    if (entry.stream) {
      entry.stream->reset();
    }
  }
  inflight_bridges.clear();
}

void CircuitRelayRuntime::AttachInflightStream(const SettledWait<CircuitRelayBridgeResult>& wait,
                                               const std::shared_ptr<Stream>& stream) {
  std::lock_guard lock(bridge_mu);
  for (auto& entry : inflight_bridges) {
    if (entry.wait.SameAs(wait)) {
      entry.stream = stream;
      return;
    }
  }
}

void CircuitRelayRuntime::CancelAllBridgesLocked() {
  for (auto& session : active_bridges) {
    if (session && session->cancelled) {
      session->cancelled->store(true, std::memory_order_release);
    }
  }
  active_bridges.clear();
}

void CircuitRelayRuntime::RemoveBridgeSessionLocked(
    const std::shared_ptr<ActiveBridgeSession>& session) {
  active_bridges.erase(std::remove_if(active_bridges.begin(), active_bridges.end(),
                                      [&](const std::shared_ptr<ActiveBridgeSession>& entry) {
                                        return entry.get() == session.get();
                                      }),
                       active_bridges.end());
}

void CircuitRelayRuntime::PruneCancelledBridgesLocked() {
  active_bridges.erase(std::remove_if(active_bridges.begin(), active_bridges.end(),
                                      [](const std::shared_ptr<ActiveBridgeSession>& entry) {
                                        return !entry || !entry->cancelled ||
                                               entry->cancelled->load(std::memory_order_acquire);
                                      }),
                       active_bridges.end());
}

void CircuitRelayRuntime::StartBridgeSession(const std::shared_ptr<Stream>& client,
                                             const std::shared_ptr<Stream>& target) {
  if (!host) {
    return;
  }
  auto session = std::make_shared<ActiveBridgeSession>();
  session->cancelled = std::make_shared<std::atomic<bool>>(false);
  const auto cancel_check = [cancelled = session->cancelled]() {
    return cancelled->load(std::memory_order_acquire);
  };
  auto self = shared_from_this();
  host->Post([self, session, client, target, cancel_check]() {
    session->to_target = std::make_shared<StreamBridge>();
    session->to_client = std::make_shared<StreamBridge>();
    auto removed = std::make_shared<std::atomic<bool>>(false);
    auto on_closed = [self, session, removed]() {
      if (removed->exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      if (session->cancelled) {
        session->cancelled->store(true, std::memory_order_release);
      }
      std::lock_guard lock(self->bridges_mu);
      self->RemoveBridgeSessionLocked(session);
    };
    session->to_target->Start(client, target, cancel_check, on_closed);
    session->to_client->Start(target, client, cancel_check, on_closed);
    std::lock_guard lock(self->bridges_mu);
    self->active_bridges.push_back(session);
  });
}

CircuitRelayBridgeResult CircuitRelayRuntime::RelayBridge(
    const Object& root, const std::shared_ptr<Stream>& client_stream) {
  CircuitRelayBridgeResult out;
  if (root.getString("op").value_or("") != "bridge") {
    out.error = "unsupported op";
    return out;
  }
  if (!host || !sessions) {
    out.error = "circuit-relay service not ready";
    return out;
  }

  std::string remote;
  if (auto peer = client_stream->remotePeerId()) {
    remote = peer.value().toBase58();
  }
  CircuitRelayAdmissionPolicy policy;
  {
    std::lock_guard lock(handler_mutex);
    policy = admission;
  }
  if (!RelayAdmissionAllowsDialer(policy.serve_scope_mask, remote, policy.contact_peer_ids)) {
    out.error = "relay scope: stranger refused";
    return out;
  }

  CircuitBridgeTarget target;
  target.target_peer_id = root.getString("target_peer_id").value_or("");
  target.target_multiaddr = root.getString("target_multiaddr").value_or("");
  target.target_protocol = root.getString("target_protocol").value_or("");

  auto normalized = NormalizeCircuitBridgeTarget(*sessions, *host, target);
  if (!normalized) {
    out.error = normalized.error().message;
    return out;
  }
  const std::string& target_peer_id = normalized->first;
  const std::string& resolved_multiaddr = normalized->second;
  out.resolved_multiaddr = resolved_multiaddr;

  const std::string target_key = target_peer_id;
  if (auto registered = sessions->RegisterEndpoint(target_key, resolved_multiaddr); !registered) {
    out.error = registered.error().message;
    return out;
  }
  sessions->ClearDialBackoff(target_key);

  const int timeout_ms = static_cast<int>(root.getNonNegInt("timeout_ms").value_or(8000));
  const auto wait_for = std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 8000);

  auto dial_promise = std::make_shared<std::promise<Roe<void>>>();
  auto dial_future = dial_promise->get_future();
  sessions->EnsureConnection(target_key, [dial_promise](Roe<void> result) {
    try {
      dial_promise->set_value(std::move(result));
    } catch (const std::future_error&) {
    }
  });
  if (!WaitReadyOrStop(dial_future, wait_for)) {
    out.error = stopping.load(std::memory_order_acquire) ? "circuit-relay aborted"
                                                         : "relay dial timed out";
    return out;
  }
  auto dialed = dial_future.get();
  if (!dialed) {
    out.error = dialed.error().message;
    return out;
  }

  libp2p::StreamProtocols protocols;
  const std::string target_protocol = root.getString("target_protocol").value_or("");
  if (!target_protocol.empty()) {
    protocols.push_back(ProtocolName{target_protocol});
  } else {
    protocols.push_back(ProtocolName{kCircuitRelayProtocolId});
  }

  auto stream_promise = std::make_shared<std::promise<libp2p::StreamAndProtocolOrError>>();
  auto stream_future = stream_promise->get_future();
  sessions->OpenStream(target_key, protocols,
                       [stream_promise](libp2p::StreamAndProtocolOrError res) {
                         try {
                           stream_promise->set_value(std::move(res));
                         } catch (const std::future_error&) {
                         }
                       });
  if (!WaitReadyOrStop(stream_future, wait_for)) {
    out.error = stopping.load(std::memory_order_acquire) ? "circuit-relay aborted"
                                                         : "relay target stream timed out";
    return out;
  }
  auto target_stream_res = stream_future.get();
  if (!target_stream_res) {
    out.error = "relay target stream failed";
    return out;
  }

  Object response;
  response.set("v", int64_t{1});
  response.set("ok", true);
  response.set("resolved_multiaddr", resolved_multiaddr);
  if (!BlockingWriteStreamJson(client_stream, DumpJson(response))) {
    out.error = "failed to ack bridge";
    return out;
  }

  StartBridgeSession(client_stream, target_stream_res.value().stream);
  out.ok = true;
  return out;
}

void CircuitRelayRuntime::HandleStream(libp2p::StreamAndProtocol stream_and_protocol) {
  auto stream = std::move(stream_and_protocol.stream);
  Libp2pHost* host_for_post = nullptr;
  {
    std::lock_guard<std::mutex> lock(handler_mutex);
    host_for_post = host;
  }
  if (!host_for_post || stopping.load(std::memory_order_acquire)) {
    if (stream) {
      stream->reset();
    }
    return;
  }
  auto self = shared_from_this();
  PostLibp2pWorker(*host_for_post, WorkerLane::Normal, [self, stream = std::move(stream)]() mutable {
    if (self->stopping.load(std::memory_order_acquire)) {
      if (stream) {
        stream->reset();
      }
      return;
    }
    CircuitRelayBridgeResult result;
    auto json_utf8 = BlockingReadStreamJson(stream);
    if (!json_utf8) {
      result.error = json_utf8.error().message;
    } else {
      auto root = TryParseObject(*json_utf8);
      if (!root) {
        result.error = "invalid circuit-relay json";
      } else {
        result = self->RelayBridge(*root, stream);
        if (result.ok) {
          return;
        }
      }
    }

    Object response;
    response.set("v", int64_t{1});
    response.set("ok", false);
    response.set("error", result.error);
    (void)BlockingWriteStreamJson(stream, DumpJson(response));
    stream->close([](auto&&) {});
  });
}

void CircuitRelayRuntime::RunClientBridgeOnWorker(const std::string& json,
                                                  SettledWait<CircuitRelayBridgeResult> wait,
                                                  libp2p::StreamAndProtocolOrError stream_res) {
  if (wait.IsSettled() || stopping.load(std::memory_order_acquire)) {
    if (stream_res) {
      stream_res.value().stream->reset();
    }
    return;
  }
  if (!stream_res) {
    wait.Finish(Error("circuit-relay stream open failed"));
    return;
  }
  auto stream = std::move(stream_res.value().stream);
  AttachInflightStream(wait, stream);
  if (wait.IsSettled()) {
    stream->reset();
    return;
  }
  if (!BlockingWriteStreamJson(stream, json)) {
    wait.Finish(Error("Failed to send circuit-relay request"));
    return;
  }
  if (wait.IsSettled()) {
    stream->reset();
    return;
  }
  auto json_utf8 = BlockingReadStreamJson(stream);
  if (wait.IsSettled()) {
    stream->reset();
    return;
  }
  if (!json_utf8) {
    wait.Finish(Error("Failed to read circuit-relay response"));
    stream->close([](auto&&) {});
    return;
  }
  auto root = TryParseObject(*json_utf8);
  if (!root) {
    wait.Finish(Error("invalid circuit-relay response"));
    stream->close([](auto&&) {});
    return;
  }
  CircuitRelayBridgeResult parsed;
  parsed.ok = root->getIf<bool>("ok").value_or(false);
  parsed.error = root->getString("error").value_or("");
  parsed.resolved_multiaddr = root->getString("resolved_multiaddr").value_or("");
  if (!parsed.ok) {
    stream->close([](auto&&) {});
    wait.Finish(parsed);
    return;
  }
  parsed.stream = stream;
  wait.Finish(parsed);
}

} // namespace pbr
