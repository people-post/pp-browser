#include "base/p2p/CallMediaAmpTransport.h"

#include <chrono>
#include <future>
#include <thread>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

} // namespace

CallMediaAmpTransport::CallMediaAmpTransport(amp::MeshRuntime& runtime, IoPump io_pump, WorkerPost post_worker)
    : coordinator_(runtime, std::move(post_worker)), io_pump_(std::move(io_pump)) {}

CallMediaAmpTransport::~CallMediaAmpTransport() {
  Stop();
}

void CallMediaAmpTransport::Start() {
  if (started_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  coordinator_.Start();
}

void CallMediaAmpTransport::Stop() {
  if (!started_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  coordinator_.Stop();
  std::lock_guard lock(mu_);
  active_leg_ = {};
  active_params_ = {};
}

void CallMediaAmpTransport::SetInboundHandler(
    std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> handler) {
  coordinator_.SetInboundHandler(
      [this, handler = std::move(handler)](CallMediaDirectConnectParams& params,
                                           CallMediaDirectCallbacks& cbs) {
        {
          std::lock_guard lock(mu_);
          active_params_ = params;
        }
        if (handler) {
          handler(params, cbs);
        }
        {
          std::lock_guard lock(mu_);
          active_params_ = params;
          // Leg id is assigned after the inbound handler returns; refresh via PrimaryLegId().
        }
      });
}

void CallMediaAmpTransport::ClearInboundHandler() {
  coordinator_.ClearInboundHandler();
}

CallMediaLegId CallMediaAmpTransport::ActiveLegId() const {
  std::lock_guard lock(mu_);
  if (active_leg_ && coordinator_.IsLegActive(active_leg_)) {
    return active_leg_;
  }
  return coordinator_.PrimaryLegId();
}

bool CallMediaAmpTransport::IsActive() const {
  return coordinator_.IsActive();
}

CallMediaDirectConnectParams CallMediaAmpTransport::ActiveParams() const {
  {
    std::lock_guard lock(mu_);
    if (!active_params_.call_id.empty()) {
      return active_params_;
    }
  }
  return coordinator_.ActiveParams();
}

CallMediaSessionPhase CallMediaAmpTransport::Phase() const {
  return coordinator_.Phase();
}

void CallMediaAmpTransport::Detach() {
  coordinator_.Detach();
  std::lock_guard lock(mu_);
  active_leg_ = {};
}

Roe<void> CallMediaAmpTransport::Connect(const CallMediaDirectConnectParams& params,
                                         CallMediaDirectCallbacks callbacks, int timeout_ms) {
  if (!started_.load(std::memory_order_acquire)) {
    return Error("amp call-media transport not started");
  }
  if (IsActive()) {
    return {};
  }

  auto result_promise = std::make_shared<std::promise<Roe<void>>>();
  auto result_future = result_promise->get_future();
  auto settled = std::make_shared<std::atomic<bool>>(false);

  auto finish = [settled, result_promise](Roe<void> value) {
    if (settled->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    try {
      result_promise->set_value(std::move(value));
    } catch (const std::future_error&) {
    }
  };

  {
    std::lock_guard lock(mu_);
    active_params_ = params;
  }

  const CallMediaLegId leg_id =
      coordinator_.StartLeg(params, std::move(callbacks),
                            [finish](Roe<void> result) { finish(std::move(result)); }, timeout_ms);
  {
    std::lock_guard lock(mu_);
    active_leg_ = leg_id;
  }

  const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 1));
  while (Clock::now() < deadline) {
    if (result_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
      break;
    }
    if (IsActive()) {
      finish({});
      break;
    }
    if (io_pump_) {
      io_pump_();
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  if (result_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    Detach();
    finish(Error("amp call-media connect timed out"));
  }
  return result_future.get();
}

Roe<void> CallMediaAmpTransport::SendAudio(const std::vector<uint8_t>& opus_payload, const uint32_t seq,
                                           const uint8_t mark) {
  const CallMediaLegId id = ActiveLegId();
  if (!id) {
    return Error("amp call-media: no active leg");
  }
  return coordinator_.SendAudio(id, opus_payload, seq, mark);
}

Roe<void> CallMediaAmpTransport::SendMedia(const uint8_t channel, const std::vector<uint8_t>& payload,
                                           const uint32_t seq, const uint8_t mark) {
  const CallMediaLegId id = ActiveLegId();
  if (!id) {
    return Error("amp call-media: no active leg");
  }
  return coordinator_.SendMedia(id, channel, payload, seq, mark);
}

} // namespace pbr
