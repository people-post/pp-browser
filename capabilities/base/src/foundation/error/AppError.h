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
  /** HTTP 429 with code "server_busy": brief_AI as a whole is busy, not this user (brief_AI contract section 8). */
  ServerBusy = 5,
  /**
   * HTTP 429 with code "stranger_quota_exceeded": a relay send without the recipient's grant spent the recipient's
   * first-contact quota for strangers (multi-relay MR005). Worth sending again later, after a backoff.
   */
  StrangerQuota = 6,
  /**
   * HTTP 401 with code "wrong_audience": the relay is not the one the command was signed for (its PeerId is another
   * than the command's `audience`, multi-relay MR007). The command is not sent again there as it is.
   */
  WrongAudience = 7,
  /**
   * No signed command goes to this relay: its identity answer named another PeerId than its address (multi-relay
   * MR008: the relay link, the build or the config), or its address names none (Me → My relays: not used).
   */
  RelayIdentityMismatch = 8,
  /** The relay claims a PeerId pinned to another origin (`relay_identity_taken`, MR007): never signed for. */
  RelayIdentityTaken = 9,
  /**
   * HTTP 409 with code "registration_mismatch" / "binding_mismatch": the revoke or unregister named a registration
   * or push binding the relay no longer holds (a newer one replaced it, MR007 item 5); nothing changed there.
   */
  ScopeMismatch = 10,
  /**
   * HTTP 426 with code "client_upgrade_required": this app is older than the relay speaks (a sign version or account
   * card it no longer takes). Final until the app is updated; the org's relay saying so shows the update screen.
   */
  ClientUpgradeRequired = 11,
};

enum class Network : int32_t {
  Generic = 0,
  Unreachable = 1,
  Timeout = 2,
  HttpError = 3,
  DnsFailed = 4,
  ConnectFailed = 5,
  TlsFailed = 6,
  /** The server refused the request itself (HTTP 4xx but 408 / 429): sending it again will not help. */
  HttpRejected = 7,
  /**
   * A relay send answered 404 `recipient_not_registered`: the recipient is not registered on this relay (it left
   * it, or never was). Final, like HttpRejected.
   */
  RecipientNotRegistered = 8,
  /**
   * A relay send answered 402 `payment_required`: the recipient charges strangers to open a conversation (its
   * initiation floor, pricing P001 / MR005), and this send carried no grant of theirs. Final until paid; the
   * detail ends `initiation_floor=<minor units>` (RelayPaymentRequiredFloor).
   */
  PaymentRequired = 9,
  /**
   * The relay's identity is not known and could not be fetched (`GET /v1/relay-identity`: unreachable, `503
   * relay_identity_unavailable`, or an answer that does not check out), so nothing can be signed for it yet
   * (multi-relay MR007). Like a relay that is down: tried again later.
   */
  RelayIdentityUnavailable = 10,
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
  UploadNotConfigured = 3,
  /** 403: the server refused the upload (signature, registration or storage target). */
  UploadRejected = 4,
  /** 404: the file or its upload is gone (expired). */
  Expired = 5,
  /** 413, or 400 "File exceeds size limit": over what the server accepts. */
  TooLarge = 6,
  /** 5xx other than "not configured": the service is not answering now. */
  Unavailable = 7,
  /** 429 "Rate limit exceeded" / "Too many pending uploads", and this device's own send limit. */
  RateLimited = 8,
  /** UploadNotConfigured after a peer-direct push to a 1:1 contact failed too: nothing could carry the file. */
  UploadNotConfiguredNoDirect = 9,
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
