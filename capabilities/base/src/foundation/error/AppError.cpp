#include "foundation/error/AppError.h"

#include "common/Localized.h"
#include "common/Logger.h"
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
      return Localized("errors.auth.register_or_rotate");
    case Err::Auth::RateLimited:
    case Err::Auth::ServerBusy:
      return Localized("errors.auth.busy");
    case Err::Auth::Generic:
    default:
      return Localized("errors.auth.failed");
    }
  case ErrorCategory::Network:
    switch (static_cast<Err::Network>(code)) {
    case Err::Network::Timeout:
      return Localized("errors.network.timeout");
    case Err::Network::DnsFailed:
      return Localized("errors.network.dns");
    case Err::Network::ConnectFailed:
      return Localized("errors.network.connect");
    case Err::Network::TlsFailed:
      return Localized("errors.network.tls");
    case Err::Network::HttpError:
    case Err::Network::HttpRejected:
      return Localized("errors.network.http");
    case Err::Network::Unreachable:
    case Err::Network::Generic:
    default:
      return Localized("errors.network.unreachable");
    }
  case ErrorCategory::Pin:
    switch (static_cast<Err::Pin>(code)) {
    case Err::Pin::Required:
      return Localized("errors.pin.unlock");
    case Err::Pin::Mismatch:
      return Localized("errors.pin.mismatch");
    case Err::Pin::TooShort:
      return Localized("errors.pin.too_short");
    case Err::Pin::VaultUnavailable:
      return Localized("errors.pin.vault");
    case Err::Pin::Generic:
    default:
      return Localized("errors.pin.required");
    }
  case ErrorCategory::Config:
    switch (static_cast<Err::Config>(code)) {
    case Err::Config::MissingKey:
      return Localized("errors.config.missing_key");
    case Err::Config::Invalid:
      return Localized("errors.config.invalid");
    case Err::Config::Generic:
    default:
      return Localized("errors.config.invalid");
    }
  case ErrorCategory::Storage:
    switch (static_cast<Err::Storage>(code)) {
    case Err::Storage::Unavailable:
      return Localized("errors.storage.unavailable");
    case Err::Storage::Failed:
      return Localized("errors.storage.failed");
    case Err::Storage::Generic:
    default:
      return Localized("errors.storage.failed");
    }
  case ErrorCategory::Blob:
    switch (static_cast<Err::Blob>(code)) {
    case Err::Blob::QuotaExceeded:
      return Localized("errors.blob.quota_exceeded");
    case Err::Blob::NothingToDelete:
      return Localized("errors.blob.quota_nothing_to_delete");
    case Err::Blob::UploadNotConfigured:
      return Localized("errors.blob.not_configured");
    case Err::Blob::UploadNotConfiguredNoDirect:
      return Localized("errors.blob.not_configured_no_direct");
    case Err::Blob::UploadRejected:
      return Localized("errors.blob.upload_rejected");
    case Err::Blob::Expired:
      return Localized("errors.blob.expired");
    case Err::Blob::TooLarge:
      return Localized("errors.blob.too_large");
    case Err::Blob::Unavailable:
      return Localized("errors.blob.unavailable");
    case Err::Blob::RateLimited:
      return Localized("errors.blob.rate_limited");
    case Err::Blob::Generic:
    default:
      return Localized("errors.blob.failed");
    }
  case ErrorCategory::Internal:
    return Localized("errors.internal");
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
  // No words for the user and none in the catalog: the message is a developer's English, not for the screen.
  // The log keeps it; the user gets a generic line in their language.
  if (!err.message.empty()) {
    logging::getLogger("AppError").warning << "No user text for error, showing the generic line: " << Log(err);
  }
  return Localized("error.generic");
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
