#include "base/error/AppError.h"

#include "base/i18n/LocalizationService.h"
#include "common/PbrCompat.h"

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

Error AppError::Blob(Err::Blob code, const std::string& detail) {
  return Error::Make(static_cast<int32_t>(ErrorCategory::Blob), static_cast<int32_t>(code), detail);
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
  case ErrorCategory::Blob:
    return "Blob";
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
      return Tr("errors.auth.register_or_rotate");
    case Err::Auth::RateLimited:
      return Tr("errors.auth.busy");
    case Err::Auth::Generic:
    default:
      return Tr("errors.auth.failed");
    }
  case ErrorCategory::Network:
    switch (static_cast<Err::Network>(code)) {
    case Err::Network::Unreachable:
    case Err::Network::Timeout:
      return Tr("errors.network.unreachable");
    case Err::Network::HttpError:
      return Tr("errors.network.http");
    case Err::Network::Generic:
    default:
      return Tr("errors.network.unreachable");
    }
  case ErrorCategory::Pin:
    switch (static_cast<Err::Pin>(code)) {
    case Err::Pin::Required:
      return Tr("errors.pin.unlock");
    case Err::Pin::Mismatch:
      return Tr("errors.pin.mismatch");
    case Err::Pin::TooShort:
      return Tr("errors.pin.too_short");
    case Err::Pin::VaultUnavailable:
      return Tr("errors.pin.vault");
    case Err::Pin::Generic:
    default:
      return Tr("errors.pin.required");
    }
  case ErrorCategory::Config:
    switch (static_cast<Err::Config>(code)) {
    case Err::Config::MissingKey:
      return Tr("errors.config.missing_key");
    case Err::Config::Invalid:
      return Tr("errors.config.invalid");
    case Err::Config::Generic:
    default:
      return Tr("errors.config.invalid");
    }
  case ErrorCategory::Storage:
    switch (static_cast<Err::Storage>(code)) {
    case Err::Storage::Unavailable:
      return Tr("errors.storage.unavailable");
    case Err::Storage::Failed:
      return Tr("errors.storage.failed");
    case Err::Storage::Generic:
    default:
      return Tr("errors.storage.failed");
    }
  case ErrorCategory::Blob:
    switch (static_cast<Err::Blob>(code)) {
    case Err::Blob::QuotaExceeded:
      return Tr("errors.blob.quota_exceeded");
    case Err::Blob::NothingToDelete:
      return Tr("errors.blob.quota_nothing_to_delete");
    case Err::Blob::Generic:
    default:
      return Tr("errors.blob.failed");
    }
  case ErrorCategory::Internal:
    return Tr("errors.internal");
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
  return Tr("errors.internal");
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
