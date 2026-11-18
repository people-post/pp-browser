#pragma once

#include "common/Error.h"
#include "foundation/error/AppError.h"
#include "gui/chat/StartupNoticePolicy.h"

#include <string>

namespace pbr {

/** Which words an account-setup failure gets. Never the raw detail: that goes to the log only. */
enum class RegistrationFailure { TooBusy, Unreachable, Other };

/**
 * The server said "too many" (HTTP 429, which the registration client keeps as a retryable HttpError with the status
 * in its detail, or a typed rate-limit), the network did not answer (unreachable class), or anything else.
 */
inline RegistrationFailure ClassifyRegistrationFailure(const Error& error) {
  const ErrorCategory category = AppError::CategoryOf(error);
  if (category == ErrorCategory::Auth) {
    const auto code = static_cast<Err::Auth>(error.code);
    if (code == Err::Auth::RateLimited || code == Err::Auth::ServerBusy) {
      return RegistrationFailure::TooBusy;
    }
    return RegistrationFailure::Other;
  }
  if (category != ErrorCategory::Network) {
    return RegistrationFailure::Other;
  }
  // The typed signal for a 429 does not reach here (the transport keeps it as a retryable HttpError with the status
  // in its text), so the text match stays.
  if (static_cast<Err::Network>(error.code) == Err::Network::HttpError) {
    return error.message.find("status 429") != std::string::npos ? RegistrationFailure::TooBusy
                                                                 : RegistrationFailure::Other;
  }
  // One definition of "can't reach the server" with the startup notices (TLS failures included).
  return IsNetworkUnreachable(error) ? RegistrationFailure::Unreachable : RegistrationFailure::Other;
}

/** The locale key of the line the setup page shows for `error`. */
inline const char* RegistrationFailureKey(const Error& error) {
  switch (ClassifyRegistrationFailure(error)) {
    case RegistrationFailure::TooBusy:
      return "onboarding.register_busy";
    case RegistrationFailure::Unreachable:
      return "onboarding.register_offline";
    case RegistrationFailure::Other:
      break;
  }
  return "onboarding.register_failed";
}

} // namespace pbr
