#pragma once

#include "domain/mesh/l4/call_media/ICallMediaTransport.h"

#include <atomic>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * An inbound hello and its answer, travelling as an event: answered exactly once — by its handler,
 * or NACKed when the last holder drops it unanswered (the path that would decide went away), so the
 * offerer retries at once instead of waiting out the leg timeout.
 */
class CallMediaInboundReply {
public:
  CallMediaInboundReply(CallMediaDirectConnectParams params, CallMediaInboundAnswer answer)
      : params_(std::move(params)), answer_(std::move(answer)) {}
  ~CallMediaInboundReply() { Answer({}, /*accept=*/false); }

  CallMediaInboundReply(const CallMediaInboundReply&) = delete;
  CallMediaInboundReply& operator=(const CallMediaInboundReply&) = delete;

  CallMediaDirectConnectParams& Params() { return params_; }
  /** Accept with `params` (its media key filled) and the bundle's callbacks. */
  void Accept(CallMediaDirectCallbacks cbs) { Answer(std::move(cbs), /*accept=*/true); }
  /** NACK (an empty media key): the offerer retries. */
  void Reject() { Answer({}, /*accept=*/false); }

private:
  void Answer(CallMediaDirectCallbacks cbs, const bool accept) {
    if (answered_.exchange(true) || !answer_) {
      return;
    }
    if (!accept) {
      params_.media_key.clear();
    }
    answer_(std::move(params_), std::move(cbs));
  }

  CallMediaDirectConnectParams params_;
  CallMediaInboundAnswer answer_;
  std::atomic<bool> answered_{false};
};

} // namespace pbr
