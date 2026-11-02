#include "lib/ai/BriefTurnRunner.h"

#include "foundation/error/AppError.h"
#include "lib/ai/LlmClient.h"
#include "common/Logger.h"

#include <optional>
#include <thread>

namespace pbr {

namespace {

template <typename Fn, typename... Args> void Call(const Fn& fn, Args&&... args) {
  if (fn) {
    fn(std::forward<Args>(args)...);
  }
}

bool IsServerBusy(const Error& error) {
  return AppError::CategoryOf(error) == ErrorCategory::Auth &&
         error.code == static_cast<int32_t>(Err::Auth::ServerBusy);
}

/** Sleeps `wait` unless the turn is cancelled first; false when it was. */
bool WaitUnlessCancelled(const std::chrono::milliseconds wait, const std::atomic<bool>& cancel) {
  const auto until = std::chrono::steady_clock::now() + wait;
  while (std::chrono::steady_clock::now() < until) {
    if (cancel.load()) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return !cancel.load();
}

} // namespace

void BriefTurnRunner::Run(const BriefAiStreamFn& stream, const BriefAiRequest& request,
                          const std::atomic<bool>& cancel, const BriefTurnSinks& sinks,
                          const std::chrono::milliseconds server_busy_wait) {
  std::multiset<std::string> shown;
  for (int attempt = 1;; ++attempt) {
    if (RunOnce(stream, request, cancel, sinks, /*may_retry=*/attempt == 1, shown) == Attempt::Finished) {
      return;
    }
    LogBusy(server_busy_wait);
    if (!WaitUnlessCancelled(server_busy_wait, cancel)) {
      Call(sinks.on_cancelled, std::string());
      return;
    }
  }
}

BriefTurnRunner::Attempt BriefTurnRunner::RunOnce(const BriefAiStreamFn& stream, const BriefAiRequest& request,
                                                  const std::atomic<bool>& cancel, const BriefTurnSinks& sinks,
                                                  const bool may_retry, std::multiset<std::string>& shown) {
  std::string text;
  // Step events stay live (a step label shows while brief_AI searches); after a busy first attempt the retry's
  // repeats of what that attempt already showed are not passed on again.
  const auto first_time = [&shown, may_retry](std::string key) {
    if (may_retry) {
      shown.insert(std::move(key));
      return true;
    }
    if (const auto it = shown.find(key); it != shown.end()) {
      shown.erase(it);
      return false;
    }
    return true;
  };
  // Set by a terminal stream event; the outcome returned by Stream() confirms it.
  std::optional<BriefAiEvent> done;
  std::optional<BriefAiEvent> error;
  std::string capability;
  std::string reason;

  const auto on_event = [&](const BriefAiEvent& event) {
    switch (event.type) {
    case BriefAiEvent::Type::Meta:
      if (first_time("meta\n" + event.route)) {
        Call(sinks.on_meta, event.route);
      }
      break;
    case BriefAiEvent::Type::Status:
      if (first_time("status\n" + event.tool + "\n" + event.phase + "\n" + event.query)) {
        Call(sinks.on_status, event.tool, event.phase, event.query);
      }
      break;
    case BriefAiEvent::Type::Token:
      text += event.delta;
      Call(sinks.on_delta, text);
      break;
    case BriefAiEvent::Type::Done:
      if (!event.route.empty()) {
        Call(sinks.on_meta, event.route); // done may carry the final (corrected) route
      }
      done = event;
      break;
    case BriefAiEvent::Type::Handoff:
      capability = event.capability;
      reason = event.reason;
      break;
    case BriefAiEvent::Type::Error:
      error = event;
      break;
    }
  };

  const Roe<BriefAiOutcome> outcome = stream(request, on_event, cancel);
  if (!outcome) {
    const bool busy = IsServerBusy(outcome.error());
    if (busy && may_retry) {
      return Attempt::ServerBusy;
    }
    // Network trouble and a busy server are worth a retry; an invalid or rate-limited key is not.
    const bool retryable = AppError::CategoryOf(outcome.error()) == ErrorCategory::Network || busy;
    Call(sinks.on_error, AppError::Display(outcome.error()), retryable, text);
    return Attempt::Finished;
  }

  switch (*outcome) {
  case BriefAiOutcome::Done: {
    const BriefAiEvent empty;
    const BriefAiEvent& event = done ? *done : empty;
    Call(sinks.on_done, event.response.empty() ? text : event.response, event.finish, event.sources, event.role);
    return Attempt::Finished;
  }
  case BriefAiOutcome::Handoff:
    Call(sinks.on_handoff, capability, reason);
    return Attempt::Finished;
  case BriefAiOutcome::Error:
    // Asked again only while nothing of an answer was shown: a retry would start the text over.
    if (may_retry && error && error->code == kServerBusyCode && text.empty()) {
      return Attempt::ServerBusy;
    }
    if (error) {
      Call(sinks.on_error, error->message, error->retryable, text);
    } else {
      Call(sinks.on_error, std::string("answer interrupted"), true, text);
    }
    return Attempt::Finished;
  case BriefAiOutcome::Cancelled:
    Call(sinks.on_cancelled, text);
    return Attempt::Finished;
  }
  return Attempt::Finished;
}

void BriefTurnRunner::LogBusy(const std::chrono::milliseconds wait) {
  static logging::Logger log = logging::getLogger("BriefTurnRunner");
  log.info << "brief_AI is busy (server_busy): asking again in " << wait.count() << " ms";
}

} // namespace pbr
