#pragma once

#include "common/Error.h"
#include "common/Value.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Signs relay command bytes with the account key (base64 out). */
using RelayAuthSigner = std::function<Roe<std::string>(const std::vector<uint8_t>&)>;

/**
 * Who a signed command to one relay is for (multi-relay MR007; docs/contracts/SERVICE_ENDPOINTS.md § The audience):
 * the relay's PeerId — the PeerId of the transport key its certificate names (release-compatibility SIGNED_FORMATS
 * § 3.4; never taken from an unsigned answer) — named in the command's bytes and body, and where the relay's refusals
 * that concern its identity or our registration there go. Every client of one relay of ours shares one
 * (RelayPins::AudienceFor); a send into a contact's relay signs for the PeerId that relay's certificate proves for the
 * relay id the contact names (RelayPins::ContactAudience). Thread-safe.
 */
class IRelayAudience {
public:
  virtual ~IRelayAudience() = default;
  /**
   * The PeerId to sign for, or why nothing may be signed for this relay now (Err::Auth::RelayIdentityMismatch,
   * RelayIdentityTaken, Err::Network::RelayIdentityUnavailable). May block: a bare origin a developer build takes is
   * asked first (MR008: every other relay's address names its PeerId).
   */
  virtual Roe<std::string> PeerId() = 0;
  /**
   * The relay refused a command signed for PeerId() with this `code`: `wrong_audience` (its identity is asked again),
   * `not_registered` / `registration_expired` (our registration there is lost), `client_upgrade_required` (this app
   * is too old for that relay), `relay_upgrade_required` (that relay is older than this app; NoteRelayRefusal names
   * an older relay's `unsupported_sign_version` so too). Any other code is ignored.
   */
  virtual void Refused(const std::string& code) = 0;
};

/**
 * A PeerId given, not proved by a certificate (tests). Refusals go to `on_refused` when set.
 */
class FixedRelayAudience : public IRelayAudience {
public:
  explicit FixedRelayAudience(std::string peer_id, std::function<void(const std::string& code)> on_refused = nullptr)
      : peer_id_(std::move(peer_id)), on_refused_(std::move(on_refused)) {}
  Roe<std::string> PeerId() override;
  void Refused(const std::string& code) override;

private:
  std::string peer_id_;
  std::function<void(const std::string&)> on_refused_;
};

/** The relay's refusal codes MR007 adds or acts on. */
inline constexpr const char kWrongAudienceCode[] = "wrong_audience";
inline constexpr const char kNotRegisteredCode[] = "not_registered";
inline constexpr const char kRegistrationExpiredCode[] = "registration_expired";
/** HTTP 426: this app is older than the relay speaks (docs/contracts/SERVICE_ENDPOINTS.md § Client compatibility). */
inline constexpr const char kClientUpgradeRequiredCode[] = "client_upgrade_required";
/**
 * HTTP 400: this relay is older than this app (a sign version or account card newer than it reads; relay U10). A
 * relay from before that code answers a newer sign version `unsupported_sign_version` (read the same: our app never
 * sends a malformed one).
 */
inline constexpr const char kRelayUpgradeRequiredCode[] = "relay_upgrade_required";
inline constexpr const char kUnsupportedSignVersionCode[] = "unsupported_sign_version";
/** HTTP 400 at register/finish: the relay did not take our account card (SPECS § register/finish). */
inline constexpr const char* kAccountCardRefusalCodes[] = {"invalid_account_card", "account_card_mismatch",
                                                           "account_card_too_large", "invalid_card_relays"};

/**
 * What a signed command's JSON body carries beside its fields: the version byte it was signed with (`sign_version`),
 * the audience, the timestamp and the signature.
 */
struct SignedRelayCommand {
  int sign_version = 0;
  std::string audience;
  int64_t timestamp = 0;
  std::string signature;
};

/** The command's bytes for the relay `audience` names (empty: they could not be built). */
using RelayCommandBytes = std::function<std::vector<uint8_t>(const std::string& audience)>;

/**
 * Signs one command for the relay `audience` names, at `timestamp`: its PeerId first (the audience's error when none
 * is in force; an error when no audience is set), then the bytes `build` makes for it, signed by `signer`.
 */
Roe<SignedRelayCommand> SignRelayCommand(IRelayAudience* audience, const RelayAuthSigner& signer, int64_t timestamp,
                                         const RelayCommandBytes& build);

/** Puts `sign_version`, `audience`, `timestamp` and `signature` into `body` (top level, beside the command's fields). */
void PutSignedRelayCommand(Object& body, const SignedRelayCommand& command);

/** The relay's machine-readable reason (`code` beside `error`) in an answer body; empty when none. */
std::string RelayRefusalCode(const std::string& body);

/**
 * A relay refused a signed command with HTTP `status` and `body`: the audience hears the codes it acts on
 * (IRelayAudience::Refused), and the typed error comes back for `401 wrong_audience` (Err::Auth::WrongAudience),
 * `403/404 not_registered` / `registration_expired` (Err::Auth::NotRegistered), `426 client_upgrade_required`
 * (Err::Auth::ClientUpgradeRequired: this app is the one to update), `400 relay_upgrade_required` or `400
 * unsupported_sign_version` (Err::Auth::RelayUpgradeRequired: the relay is the one to update; the audience hears
 * `relay_upgrade_required`) and a `400` account-card code (Err::Auth::AccountCardRefused, naming the code); nullopt
 * for any other refusal (the caller words it as before). Every relay client (relay, registration, blob, locker, push)
 * reads its refusals through this one.
 */
std::optional<Error> NoteRelayRefusal(IRelayAudience* audience, long status, const std::string& body);

/** `error` is a relay's `426 client_upgrade_required` (NoteRelayRefusal). */
bool IsClientUpgradeRequired(const Error& error);
/** `error` is a relay's `400 relay_upgrade_required` / `unsupported_sign_version` (NoteRelayRefusal). */
bool IsRelayUpgradeRequired(const Error& error);
/**
 * A refusal that asking again will not change until one side is updated or the relay's identity is fixed (T14):
 * ClientUpgradeRequired, RelayUpgradeRequired, WrongAudience. Callers that repeat a call (the inbox poll, pp-node's
 * heartbeat and registration, other-relay registration) slow down after one.
 */
bool IsPermanentRelayRefusal(const Error& error);

} // namespace pbr
