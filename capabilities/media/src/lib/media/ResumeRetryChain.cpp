#include "lib/media/ResumeRetryChain.h"

namespace pbr {

bool ResumeRetryChain::OnBegan(const TimePoint now) {
  began_ = true;
  began_at_ = now;
  tries_ = 0;
  failures_ = 0;
  skips_ = 0;
  if (running_) {
    return false;
  }
  running_ = true;
  return true;
}

ResumeRetryChain::TickDecision ResumeRetryChain::OnTick(const bool session_active, const bool interrupted,
                                                        const AppState app_state) {
  TickDecision decision;
  if (!running_ || !session_active) {
    End();
    decision.stop = StopReason::SessionEnded;
    return decision;
  }
  if (!interrupted) {
    End();
    decision.stop = StopReason::ResumedElsewhere;
    return decision;
  }
  if (app_state == AppState::Inactive) {
    ++skips_;
    decision.action = Action::Skip;
    decision.skips = skips_;
    decision.log_skip = ShouldLog(skips_);
    return decision;
  }
  ++tries_;
  decision.action = Action::Try;
  return decision;
}

ResumeRetryChain::TryOutcome ResumeRetryChain::OnTryResult(const TryResult result, const TimePoint now) {
  TryOutcome outcome;
  switch (result) {
    case TryResult::Ok:
      End();
      outcome.stop = StopReason::Resumed;
      outcome.tries = tries_;
      outcome.waited = SinceBegan(now);
      began_ = false;
      break;
    case TryResult::Failed:
      ++failures_;
      outcome.reschedule = true;
      outcome.failures = failures_;
      outcome.log_failure = ShouldLog(failures_);
      break;
    case TryResult::Stale:
      outcome.reschedule = true;
      break;
    case TryResult::Gone:
      End();
      outcome.stop = StopReason::SessionEnded;
      break;
    case TryResult::Already:
      End();
      outcome.stop = StopReason::ResumedElsewhere;
      break;
  }
  return outcome;
}

std::chrono::milliseconds ResumeRetryChain::SinceBegan(const TimePoint now) const {
  if (!began_ || now < began_at_) {
    return std::chrono::milliseconds{0};
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(now - began_at_);
}

bool ResumeRetryChain::ShouldLog(const int count) {
  return count >= 1 && (count - 1) % 10 == 0;
}

const char* ResumeRetryChain::Word(const StopReason reason) {
  switch (reason) {
    case StopReason::Resumed:
      return "resumed";
    case StopReason::SessionEnded:
      return "session_ended";
    case StopReason::ResumedElsewhere:
      return "resumed_elsewhere";
    case StopReason::None:
      break;
  }
  return "none";
}

const char* ResumeRetryChain::Word(const AppState state) {
  switch (state) {
    case AppState::Active:
      return "active";
    case AppState::Inactive:
      return "inactive";
    case AppState::Background:
      return "background";
    case AppState::Unknown:
      break;
  }
  return "unknown";
}

} // namespace pbr
