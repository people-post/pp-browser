#include "base/mesh/reachability/AmpDialBackService.h"

#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/AdpMultiaddr.h"
#include "common/SettledWait.h"
#include "common/ValueJson.h"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

using Clock = std::chrono::steady_clock;

std::vector<uint8_t> JsonToBody(const std::string& json_utf8) {
  return std::vector<uint8_t>(json_utf8.begin(), json_utf8.end());
}

void RunWorker(const AmpDialBackService::WorkerPost& post_worker, std::function<void()> task) {
  if (post_worker) {
    post_worker(std::move(task));
  } else {
    task();
  }
}

std::chrono::milliseconds RemainingTimeout(const Clock::time_point deadline) {
  const auto now = Clock::now();
  if (now >= deadline) {
    return std::chrono::milliseconds(1);
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

DialBackProbeResult DialAmpTargets(pp::amp::PeerLinkManager& links, AmpDialBackService::IoPump io_pump,
                                   const std::vector<std::string>& targets, int timeout_ms) {
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
    if (!pp::amp::ParseAdpMultiaddr(ma)) {
      out.error = "target is not an ADP multiaddr";
      out.dialed = ma;
      continue;
    }
    const std::string key = "dialback:probe:" + std::to_string(i);
    if (auto registered = links.RegisterEndpoint(key, ma); !registered) {
      out.error = registered.error().message;
      out.dialed = ma;
      continue;
    }

    SettledWait<void> wait;
    links.EnsureAssociation(key, [wait](pp::amp::PeerLinkManager::LinkRoe result) {
      if (result) {
        wait.Finish(Roe<void>());
      } else {
        wait.Finish(Roe<void>(Error(result.error().message)));
      }
    });
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
    while (Clock::now() < deadline && !wait.IsSettled()) {
      if (io_pump) {
        io_pump();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    auto dialed = wait.Wait(std::chrono::milliseconds(1), Error("dial-back timed out"));
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

struct AmpDialBackService::Impl {
  pp::amp::PeerLinkManager* links = nullptr;
  IoPump io_pump;
  WorkerPost post_worker;
  std::atomic<bool> stopped{false};

  void IoPumpUntil(const std::function<bool()>& done, const Clock::time_point deadline) {
    while (!done() && Clock::now() < deadline) {
      if (io_pump) {
        io_pump();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }

  void HandleInboundOnLink(pp::amp::PeerLink& link, const uint32_t channel_id) {
    if (stopped.load(std::memory_order_acquire) || !links) {
      return;
    }
    auto session = std::make_shared<pp::amp::ChannelSession>();
    session->Bind(*link.Mux(), channel_id, pp::amp::ControlJsonChannelPolicy(),
                  [this, session](Roe<std::vector<uint8_t>> frame) {
                    if (!frame || stopped.load(std::memory_order_acquire)) {
                      return false;
                    }
                    auto body = std::move(*frame);
                    RunWorker(post_worker, [this, session, body = std::move(body)]() mutable {
                      if (stopped.load(std::memory_order_acquire) || !links) {
                        return;
                      }
                      DialBackProbeResult result;
                      const std::string json_utf8(body.begin(), body.end());
                      auto root = TryParseObject(json_utf8);
                      if (!root) {
                        result.error = "invalid dial-back json";
                      } else {
                        const std::string op = root->getString("op").value_or("");
                        if (op != "probe") {
                          result.error = "unsupported op";
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
                          result = DialAmpTargets(*links, io_pump, targets, timeout_ms);
                        }
                      }
                      Object response;
                      response.set("v", int64_t{1});
                      response.set("ok", result.ok);
                      response.set("dialed", result.dialed);
                      response.set("error", result.error);
                      (void)session->EnqueueOutbound(JsonToBody(DumpJson(response)));
                      if (io_pump) {
                        io_pump();
                      }
                      session->Close();
                    });
                    return false;
                  });
  }
};

AmpDialBackService::AmpDialBackService(pp::amp::PeerLinkManager& links, IoPump io_pump, WorkerPost post_worker)
    : impl_(std::make_unique<Impl>()), links_(links), io_pump_(std::move(io_pump)),
      post_worker_(std::move(post_worker)) {
  impl_->links = &links_;
  impl_->io_pump = io_pump_;
  impl_->post_worker = post_worker_;
}

AmpDialBackService::~AmpDialBackService() { Stop(); }

void AmpDialBackService::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  impl_->stopped.store(false, std::memory_order_release);
  links_.SetProtocolHandler(kDialBackProtocolId,
                            [impl = impl_.get()](pp::amp::PeerLink& link, const uint32_t channel_id) {
                              impl->HandleInboundOnLink(link, channel_id);
                            });
}

void AmpDialBackService::Stop() {
  started_ = false;
  impl_->stopped.store(true, std::memory_order_release);
  links_.RemoveProtocolHandler(kDialBackProtocolId);
}

Roe<DialBackProbeResult> AmpDialBackService::Probe(const std::string& seed_peer_key,
                                                   const std::vector<std::string>& target_multiaddrs,
                                                   int timeout_ms) {
  if (!started_) {
    return Error("amp dial-back service not started");
  }
  if (!links_.GetLinkSnapshot(seed_peer_key).has_endpoint) {
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
  const std::string request_json = DumpJson(request);

  const int wait_ms = (timeout_ms > 0 ? timeout_ms : 8000) + 2000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);

  SettledWait<DialBackProbeResult> wait;
  auto session = std::make_shared<pp::amp::ChannelSession>();
  auto settled = std::make_shared<std::atomic<bool>>(false);

  auto finish = [settled, wait, session](Roe<DialBackProbeResult> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    session->Close();
    wait.Finish(std::move(value));
  };

  const auto read_timeout = RemainingTimeout(deadline);
  links_.EnsureAssociation(seed_peer_key, [this, seed_peer_key, request_json, finish, session, deadline,
                                           read_timeout](pp::amp::PeerLinkManager::LinkRoe assoc) mutable {
    if (!assoc) {
      finish(Error(assoc.error().message));
      return;
    }
    links_.OpenChannel(seed_peer_key, kDialBackProtocolId, pp::amp::ControlJsonChannelPolicy(read_timeout),
                       [this, seed_peer_key, request_json, finish, session, deadline,
                        read_timeout](pp::amp::PeerLinkManager::ChannelRoe channel) mutable {
                         if (!channel) {
                           finish(Error(channel.error().message));
                           return;
                         }
                         impl_->IoPumpUntil(
                             [&] {
                               auto* link = links_.FindLink(seed_peer_key);
                               return link && link->Mux() &&
                                      link->Mux()->State(*channel) == pp::amp::ChannelState::Open;
                             },
                             deadline);
                         auto* link = links_.FindLink(seed_peer_key);
                         if (!link || !link->Mux() ||
                             link->Mux()->State(*channel) != pp::amp::ChannelState::Open) {
                           finish(Error("amp dial-back: channel open failed"));
                           return;
                         }
                         session->Bind(*link->Mux(), *channel, pp::amp::ControlJsonChannelPolicy(read_timeout),
                                       [finish](Roe<std::vector<uint8_t>> frame) {
                                         if (!frame) {
                                           finish(Error("Failed to read dial-back response"));
                                           return false;
                                         }
                                         auto root = TryParseObject(std::string(frame->begin(), frame->end()));
                                         if (!root) {
                                           finish(Error("invalid dial-back response"));
                                           return false;
                                         }
                                         DialBackProbeResult parsed;
                                         parsed.ok = root->getIf<bool>("ok").value_or(false);
                                         parsed.dialed = root->getString("dialed").value_or("");
                                         parsed.error = root->getString("error").value_or("");
                                         finish(parsed);
                                         return false;
                                       });
                         if (!session->EnqueueOutbound(JsonToBody(request_json))) {
                           finish(Error("Failed to send dial-back probe"));
                           return;
                         }
                         if (io_pump_) {
                           io_pump_();
                         }
                       });
  });

  while (Clock::now() < deadline && !wait.IsSettled()) {
    if (io_pump_) {
      io_pump_();
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return wait.Wait(std::chrono::milliseconds(1), Error("dial-back probe timed out"));
}

} // namespace pbr
