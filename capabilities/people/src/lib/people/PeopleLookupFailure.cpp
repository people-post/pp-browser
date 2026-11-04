#include "lib/people/PeopleLookupFailure.h"

#include "foundation/error/AppError.h"

#include <string>

namespace pbr {

namespace {

// OrgBackendClientsImpl.cpp builds these as plain-text Errors without a category/code.
constexpr const char* kAccountNotFound = "Account not found";
constexpr const char* kRelayUserNotFound = "Relay user not found";
constexpr const char* kStatusMarker = " failed with status ";

bool IsServerErrorStatusText(const std::string& message) {
  const size_t at = message.find(kStatusMarker);
  if (at == std::string::npos) {
    return false;
  }
  const size_t digit = at + std::string(kStatusMarker).size();
  return digit < message.size() && message[digit] == '5';
}

} // namespace

PeopleLookupFailure ClassifyPeopleLookupError(const Error& error) {
  // HttpClient reports transport failures (curl) as AppError::Network with a code naming the cause.
  if (AppError::CategoryOf(error) == ErrorCategory::Network) {
    return PeopleLookupFailure::DirectoryUnreachable;
  }
  if (error.message == kAccountNotFound || error.message == kRelayUserNotFound) {
    return PeopleLookupFailure::NotFound;
  }
  if (IsServerErrorStatusText(error.message)) {
    return PeopleLookupFailure::DirectoryUnreachable;
  }
  return PeopleLookupFailure::Other;
}

} // namespace pbr
