#pragma once

#include "foundation/error/AppError.h"

#include <cstdint>

namespace pbr {

/** How long after launch a background network failure stays quiet if nothing has succeeded yet. */
inline constexpr int64_t kStartupQuietWindowMs = 30'000;

/**
 * A failure of "the network is not usable" kind: no answer came (no DNS, no route, no connection, timed out, the relay
 * identity could not be fetched). A server that answered, even with an error, is not one.
 */
inline bool IsNetworkUnreachable(const Error& err) {
  if (AppError::CategoryOf(err) != ErrorCategory::Network) {
    return false;
  }
  switch (static_cast<Err::Network>(err.code)) {
    case Err::Network::Generic:
    case Err::Network::Unreachable:
    case Err::Network::Timeout:
    case Err::Network::DnsFailed:
    case Err::Network::ConnectFailed:
    case Err::Network::TlsFailed: // no usable connection to the server: the user reads "can't reach it"
    case Err::Network::RelayIdentityUnavailable:
      return true;
    case Err::Network::HttpError:
    case Err::Network::HttpRejected:
    case Err::Network::RecipientNotRegistered:
    case Err::Network::PaymentRequired:
      return false;
  }
  return false;
}

/**
 * Whether a failure the user did not ask for is put in front of them. Right after launch on a fresh install the
 * system's network / local-network prompts can still be open and nothing resolves for a while; saying "request
 * failed" over that is noise, and the request is tried again by its own retry. Quiet only for an unreachable network,
 * only for background work, only until the first success or the window's end. Whatever the user just did always shows.
 */
inline bool ShowBackgroundFailure(const Error& err, const bool user_initiated, const int64_t ms_since_launch,
                                  const bool had_first_success) {
  if (user_initiated) {
    return true;
  }
  if (!IsNetworkUnreachable(err)) {
    return true;
  }
  return had_first_success || ms_since_launch >= kStartupQuietWindowMs;
}

} // namespace pbr
