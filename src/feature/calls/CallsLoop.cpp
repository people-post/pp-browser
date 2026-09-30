#include "feature/calls/CallsLoop.h"

#include "common/Logger.h"

#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

logging::Logger& CallsLoopLog() {
  static logging::Logger logger = logging::getLogger("CallsLoop");
  return logger;
}

} // namespace

const char* CallStackEventName(const CallStackEvent& event) {
  return std::visit(
      [](const auto& e) -> const char* {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, calls_event::LocalNetworkChanged>) {
          return "LocalNetworkChanged";
        } else if constexpr (std::is_same_v<E, calls_event::ObservedAddressChanged>) {
          return "ObservedAddressChanged";
        } else if constexpr (std::is_same_v<E, calls_event::MobilityOverrideChanged>) {
          return "MobilityOverrideChanged";
        } else if constexpr (std::is_same_v<E, calls_event::MobilityWake>) {
          return "MobilityWake";
        } else if constexpr (std::is_same_v<E, calls_event::CallControlReceived>) {
          return "CallControlReceived";
        } else if constexpr (std::is_same_v<E, calls_event::RelayChosen>) {
          return "RelayChosen";
        } else if constexpr (std::is_same_v<E, calls_event::SignalingPunchRequested>) {
          return "SignalingPunchRequested";
        } else {
          static_assert(!sizeof(E), "name every CallStackEvent");
        }
      },
      event);
}

void CallsLoop::Enqueue(CallStackEvent event) {
  // shared_ptr: the posted task must be copyable (std::function); the event may hold move-only data.
  tasks_.Post([this, event = std::make_shared<CallStackEvent>(std::move(event))]() { Handle(*event); });
}

void CallsLoop::EnqueueFront(CallStackEvent event) {
  tasks_.PostFront([this, event = std::make_shared<CallStackEvent>(std::move(event))]() { Handle(*event); });
}

CallsExecutor::TimerId CallsLoop::After(const std::chrono::milliseconds delay, CallStackEvent event) {
  return tasks_.After(delay, [this, event = std::make_shared<CallStackEvent>(std::move(event))]() { Handle(*event); });
}

void CallsLoop::Handle(CallStackEvent& event) {
  CallsLoopLog().debug << "event " << CallStackEventName(event);
  if (handler_) {
    handler_(event);
  }
}

void CallsWakeSlot::ArmAt(const std::optional<Clock::time_point> at) {
  if (at == at_ && (timer_ != 0 || !at)) {
    return;  // already armed there (or already disarmed)
  }
  Disarm();
  if (!at) {
    return;
  }
  at_ = at;
  const auto delay =
      std::max(std::chrono::duration_cast<std::chrono::milliseconds>(*at - Clock::now()), std::chrono::milliseconds(1));
  timer_ = loop_.After(delay, wake_);
}

void CallsWakeSlot::Disarm() {
  loop_.Cancel(timer_);
  at_.reset();
}

void CallsWakeSlot::Fired() {
  timer_ = 0;
  at_.reset();
}

} // namespace pbr
