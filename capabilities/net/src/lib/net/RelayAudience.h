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
 * the relay's PeerId, named in the command's bytes and body, and where the relay's refusals that concern its identity
 * or our registration there go. Every client of one relay of ours shares one (RelayPins::AudienceFor); a send into a
 * contact's relay names the PeerId its card signed (FixedRelayAudience). Thread-safe.
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
   * `not_registered` / `registration_expired` (our registration there is lost). Any other code is ignored.
   */
  virtual void Refused(const std::string& code) = 0;
};

/** A PeerId given, not pinned (a contact's card or QR code names it; tests). Refusals go to `on_refused` when set. */
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
 * (IRelayAudience::Refused), and the typed error comes back for `401 wrong_audience` (Err::Auth::WrongAudience) and
 * `403/404 not_registered` / `registration_expired` (Err::Auth::NotRegistered); nullopt for any other refusal (the
 * caller words it as before).
 */
std::optional<Error> NoteRelayRefusal(IRelayAudience* audience, long status, const std::string& body);

} // namespace pbr
