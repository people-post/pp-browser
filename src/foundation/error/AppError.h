#pragma once

#include "common/Error.h"

#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** App-level error categories (stored as int32_t on Error::category). */
enum class ErrorCategory : int32_t {
  Unknown = 0,
  Internal = 1,
  Auth = 2,
  Network = 3,
  Pin = 4,
  Config = 5,
  Storage = 6,
  Blob = 7,
};

/** Fine-grained codes scoped by category (stored as int32_t on Error::code). */
namespace Err {

enum class Auth : int32_t {
  Generic = 0,
  NotRegistered = 1,
  Forbidden = 2,
  Expired = 3,
  RateLimited = 4,
};

enum class Network : int32_t {
  Generic = 0,
  Unreachable = 1,
  Timeout = 2,
  HttpError = 3,
};

enum class Pin : int32_t {
  Generic = 0,
  Required = 1,
  Mismatch = 2,
  TooShort = 3,
  VaultUnavailable = 4,
};

enum class Config : int32_t {
  Generic = 0,
  MissingKey = 1,
  Invalid = 2,
};

enum class Storage : int32_t {
  Generic = 0,
  Unavailable = 1,
  Failed = 2,
};

enum class Internal : int32_t {
  Generic = 0,
};

enum class Blob : int32_t {
  Generic = 0,
  QuotaExceeded = 1,
  NothingToDelete = 2,
};

} // namespace Err

/** Typed constructors and UI/log helpers over low-level Error. */
struct AppError {
  static Error Auth(Err::Auth code, const std::string& detail);
  static Error Network(Err::Network code, const std::string& detail);
  static Error Pin(Err::Pin code, const std::string& detail);
  static Error Config(Err::Config code, const std::string& detail);
  static Error Storage(Err::Storage code, const std::string& detail);
  static Error Blob(Err::Blob code, const std::string& detail);
  static Error Internal(const std::string& detail);

  static ErrorCategory CategoryOf(const Error& err);
  static std::string CatalogMessage(ErrorCategory category, int32_t code);
  /** user → catalog(category, code) → message → generic. */
  static std::string Display(const Error& err);
  static std::string Log(const Error& err);
  static const char* CategoryName(ErrorCategory category);
};

} // namespace pbr
