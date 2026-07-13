#include "base/error/AppError.h"

namespace pbr {

Error AppError::Auth(Err::Auth code, const std::string& detail) {
  return Error::Make(static_cast<int32_t>(ErrorCategory::Auth), static_cast<int32_t>(code), detail);
}

Error AppError::Network(Err::Network code, const std::string& detail) {
  return Error::Make(static_cast<int32_t>(ErrorCategory::Network), static_cast<int32_t>(code), detail);
}

Error AppError::Pin(Err::Pin code, const std::string& detail) {
  return Error::Make(static_cast<int32_t>(ErrorCategory::Pin), static_cast<int32_t>(code), detail);
}

Error AppError::Config(Err::Config code, const std::string& detail) {
  return Error::Make(static_cast<int32_t>(ErrorCategory::Config), static_cast<int32_t>(code), detail);
}

Error AppError::Storage(Err::Storage code, const std::string& detail) {
  return Error::Make(static_cast<int32_t>(ErrorCategory::Storage), static_cast<int32_t>(code), detail);
}

Error AppError::Internal(const std::string& detail) {
  return Error::Make(static_cast<int32_t>(ErrorCategory::Internal),
                     static_cast<int32_t>(Err::Internal::Generic), detail);
}

ErrorCategory AppError::CategoryOf(const Error& err) {
  return static_cast<ErrorCategory>(err.category);
}

const char* AppError::CategoryName(ErrorCategory category) {
  switch (category) {
  case ErrorCategory::Auth:
    return "Auth";
  case ErrorCategory::Network:
    return "Network";
  case ErrorCategory::Pin:
    return "Pin";
  case ErrorCategory::Config:
    return "Config";
  case ErrorCategory::Storage:
    return "Storage";
  case ErrorCategory::Internal:
    return "Internal";
  case ErrorCategory::Unknown:
  default:
    return "Unknown";
  }
}

std::string AppError::CatalogMessage(ErrorCategory category, int32_t code) {
  switch (category) {
  case ErrorCategory::Auth:
    switch (static_cast<Err::Auth>(code)) {
    case Err::Auth::NotRegistered:
    case Err::Auth::Forbidden:
    case Err::Auth::Expired:
      return "Register or rotate your Brief API key in Me → Profile.";
    case Err::Auth::RateLimited:
      return "Service is busy — try again shortly.";
    case Err::Auth::Generic:
    default:
      return "Authentication failed — check Me → Profile / Assistant.";
    }
  case ErrorCategory::Network:
    switch (static_cast<Err::Network>(code)) {
    case Err::Network::Unreachable:
    case Err::Network::Timeout:
      return "Can't reach the server — check network or Me → Network / Assistant.";
    case Err::Network::HttpError:
      return "Request failed — try again, or check Me → Assistant.";
    case Err::Network::Generic:
    default:
      return "Can't reach the server — check network or Me → Network / Assistant.";
    }
  case ErrorCategory::Pin:
    switch (static_cast<Err::Pin>(code)) {
    case Err::Pin::Required:
      return "Unlock your profile PIN to continue.";
    case Err::Pin::Mismatch:
      return "New PINs do not match";
    case Err::Pin::TooShort:
      return "Use at least 4 characters";
    case Err::Pin::VaultUnavailable:
      return "Vault unavailable";
    case Err::Pin::Generic:
    default:
      return "PIN required to continue";
    }
  case ErrorCategory::Config:
    switch (static_cast<Err::Config>(code)) {
    case Err::Config::MissingKey:
      return "Configure the assistant in Me → Assistant.";
    case Err::Config::Invalid:
      return "Check settings in Me and try again.";
    case Err::Config::Generic:
    default:
      return "Check settings in Me and try again.";
    }
  case ErrorCategory::Storage:
    switch (static_cast<Err::Storage>(code)) {
    case Err::Storage::Unavailable:
      return "Profile data unavailable";
    case Err::Storage::Failed:
      return "Couldn't update local data — try again.";
    case Err::Storage::Generic:
    default:
      return "Couldn't update local data — try again.";
    }
  case ErrorCategory::Internal:
    return "Something went wrong — try again.";
  case ErrorCategory::Unknown:
  default:
    return {};
  }
}

std::string AppError::Display(const Error& err) {
  if (!err.user.empty()) {
    return err.user;
  }
  std::string catalogued = CatalogMessage(CategoryOf(err), err.code);
  if (!catalogued.empty()) {
    return catalogued;
  }
  if (!err.message.empty()) {
    return err.message;
  }
  return "Something went wrong — try again.";
}

std::string AppError::Log(const Error& err) {
  std::string out = "[";
  out += CategoryName(CategoryOf(err));
  out += "/";
  out += std::to_string(err.code);
  out += "]";
  if (!err.user.empty()) {
    out += " user=\"";
    out += err.user;
    out += "\"";
  }
  if (!err.message.empty()) {
    out += " detail=\"";
    out += err.message;
    out += "\"";
  }
  return out;
}

} // namespace pbr
