#pragma once

#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "foundation/runtime/AppRuntime.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace pbr::test {

/**
 * Test side of the async inbound hello (CallMediaInboundHandler, t3): fakes store the product's
 * handler here and tests deliver hellos through it. The handler answers later — typically from
 * the calls owner — so `DeliverAndWait` drains UI + owners until the answer lands.
 */
class InboundHelloFake {
public:
  struct Answer {
    std::mutex mu;
    bool answered = false;
    CallMediaDirectConnectParams params;
    CallMediaDirectCallbacks cbs;
  };

  void Set(CallMediaInboundHandler handler) {
    std::lock_guard lock(mu_);
    handler_ = std::move(handler);
  }
  void Clear() { Set({}); }
  bool Installed() const {
    std::lock_guard lock(mu_);
    return static_cast<bool>(handler_);
  }

  /** Deliver a hello the way the transport's IO hop does; the answer fills the returned state. */
  std::shared_ptr<Answer> Deliver(CallMediaDirectConnectParams params) const {
    CallMediaInboundHandler handler;
    {
      std::lock_guard lock(mu_);
      handler = handler_;
    }
    if (!handler) {
      return nullptr;
    }
    auto answer = std::make_shared<Answer>();
    handler(std::move(params), [answer](CallMediaDirectConnectParams p, CallMediaDirectCallbacks c) {
      std::lock_guard lock(answer->mu);
      answer->answered = true;
      answer->params = std::move(p);
      answer->cbs = std::move(c);
    });
    return answer;
  }

  /** Deliver; `then` runs with the answer, on whichever thread answers. False without a handler. */
  bool DeliverThen(CallMediaDirectConnectParams params, CallMediaInboundAnswer then) const {
    CallMediaInboundHandler handler;
    {
      std::lock_guard lock(mu_);
      handler = handler_;
    }
    if (!handler) {
      return false;
    }
    handler(std::move(params), std::move(then));
    return true;
  }

  static bool Answered(const std::shared_ptr<Answer>& answer) {
    std::lock_guard lock(answer->mu);
    return answer->answered;
  }

  /**
   * Deliver and pump UI + manual owners until answered or `budget` runs out; copies the answer
   * into `params` / `cbs`. False when no handler is installed or no answer came in time.
   */
  bool DeliverAndWait(CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs,
                      std::chrono::milliseconds budget = std::chrono::seconds(3)) const {
    auto answer = Deliver(params);
    if (!answer) {
      return false;
    }
    const auto until = std::chrono::steady_clock::now() + budget;
    while (!Answered(answer) && std::chrono::steady_clock::now() < until) {
      AppRuntime::RunUIAndOwnerTasks();
      if (!Answered(answer)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    std::lock_guard lock(answer->mu);
    if (!answer->answered) {
      return false;
    }
    params = answer->params;
    cbs = answer->cbs;
    return true;
  }

private:
  mutable std::mutex mu_;
  CallMediaInboundHandler handler_;
};

} // namespace pbr::test
