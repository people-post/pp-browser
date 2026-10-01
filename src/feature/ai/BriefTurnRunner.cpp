#include "feature/ai/BriefTurnRunner.h"

#include "foundation/error/AppError.h"

#include <optional>

namespace pbr {

namespace {

template <typename Fn, typename... Args> void Call(const Fn& fn, Args&&... args) {
  if (fn) {
    fn(std::forward<Args>(args)...);
  }
}

} // namespace

void BriefTurnRunner::Run(const BriefAiStreamFn& stream, const BriefAiRequest& request,
                          const std::atomic<bool>& cancel, const BriefTurnSinks& sinks) {
  std::string text;
  // Set by a terminal stream event; the outcome returned by Stream() confirms it.
  std::optional<BriefAiEvent> done;
  std::optional<BriefAiEvent> error;

  const auto on_event = [&](const BriefAiEvent& event) {
    switch (event.type) {
    case BriefAiEvent::Type::Meta:
      break;
    case BriefAiEvent::Type::Status:
      Call(sinks.on_status, event.tool, event.phase, event.query);
      break;
    case BriefAiEvent::Type::Token:
      text += event.delta;
      Call(sinks.on_delta, text);
      break;
    case BriefAiEvent::Type::Done:
      done = event;
      break;
    case BriefAiEvent::Type::Handoff:
      break;
    case BriefAiEvent::Type::Error:
      error = event;
      break;
    }
  };

  const Roe<BriefAiOutcome> outcome = stream(request, on_event, cancel);
  if (!outcome) {
    Call(sinks.on_error, AppError::Display(outcome.error()), true, text);
    return;
  }

  switch (*outcome) {
  case BriefAiOutcome::Done: {
    const BriefAiEvent empty;
    const BriefAiEvent& event = done ? *done : empty;
    Call(sinks.on_done, event.response.empty() ? text : event.response, event.finish, event.sources);
    return;
  }
  case BriefAiOutcome::Handoff:
    Call(sinks.on_handoff);
    return;
  case BriefAiOutcome::Error:
    if (error) {
      Call(sinks.on_error, error->message, error->retryable, text);
    } else {
      Call(sinks.on_error, std::string("answer interrupted"), true, text);
    }
    return;
  case BriefAiOutcome::Cancelled:
    Call(sinks.on_cancelled, text);
    return;
  }
}

} // namespace pbr
