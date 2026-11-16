#pragma once

#include "common/Error.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What a relay of ours last answered our registration with (kept: Me → My relays shows it after a restart). */
struct RelayRegistrationRecord {
  /**
   * Unknown: not asked yet (registering). NotRegistered: the identity holds no registration there (the preferred
   * relay: the person has not registered, or registered elsewhere). Expired: registered until a time now past.
   */
  enum class State { Unknown, Registered, Failed, NotRegistered, Expired };
  State state = State::Unknown;
  /** Failed: why, as a code (RelayRegistrationFailureCode: unreachable, refused, identity, other). */
  std::string reason;
  /** Registered: until when (ISO-8601, the relay's `expires_at`). */
  std::string expires_at;
  /** Registered: the `issued_at` of the card it took (a newer card registers there again). */
  int64_t card_issued_at = 0;
  /** Registered: the registration's id there (MR007: a revoke names it). */
  std::string registration_id;
  /** When it answered (Unix ms). */
  int64_t at_ms = 0;

  bool operator==(const RelayRegistrationRecord&) const = default;
};

/**
 * One relay of ours: its address — its base URL and the PeerId it must answer to (multi-relay MR008: from the relay
 * link it was added by, the build's or the config's for the org's relay, our own card on a linked device; empty only
 * for a bare origin, which a developer build checks on first use, or the org's relay in a build that ships none) —
 * and its last registration.
 */
struct OurRelayEntry {
  std::string origin;
  std::string peer_id;
  RelayRegistrationRecord registration;

  bool operator==(const OurRelayEntry&) const = default;
};

/** The codes a failed registration's `reason` holds. */
inline constexpr const char kRegistrationUnreachable[] = "unreachable";
inline constexpr const char kRegistrationRefused[] = "refused";
inline constexpr const char kRegistrationIdentity[] = "identity";
inline constexpr const char kRegistrationOther[] = "other";
/**
 * Why a registration failed, as a code the page words: unreachable (no answer: a transport failure, a timeout, a
 * 5xx), refused (the relay answered no), identity (its identity is not in force: changed, taken or not given), other.
 */
std::string RelayRegistrationFailureCode(const Error& error);

/** My relays (multi-relay R3, MR006): ours, preferred first. */
struct OurRelaysList {
  std::vector<OurRelayEntry> relays;

  std::vector<std::string> Origins() const;
  /** The preferred relay's base URL ("" when none). */
  std::string Preferred() const;
  bool Lists(const std::string& origin) const;
  bool operator==(const OurRelaysList&) const = default;
};

/** Why a change to the list was refused (the code is the Error's message: the settings presenter words it). */
enum class RelayListProblem { None, AlreadyListed, Full, NotListed, PreferredNotRemovable, LastRelay, NotMainDevice };
/** The stable code of `problem` (e.g. "relay_list_full"); "" for None. */
const char* RelayListProblemCode(RelayListProblem problem);
/** The problem a code names (None for any other text). */
RelayListProblem RelayListProblemFromCode(const std::string& code);

/** `list` with `base_url` (named `peer_id`) added last (a relay of ours already, or a fifth: refused). */
Roe<OurRelaysList> WithRelayAdded(const OurRelaysList& list, const std::string& base_url, const std::string& peer_id);
/** `list` without `origin`: never the preferred one (mark another first), never the last. */
Roe<OurRelaysList> WithRelayRemoved(const OurRelaysList& list, const std::string& origin);
/** `list` with `origin` first, the others in their order. */
Roe<OurRelaysList> WithRelayPreferred(const OurRelaysList& list, const std::string& origin);

/**
 * The list of the profile (docs/contracts/DATA_LAYOUT.md `our_relays.json`): user data, seeded once from the
 * deployment's relay and config `relay.extra_base_urls` (OurRelayOrigins); afterwards the config is only a new
 * profile's default. Each relay keeps its last registration. Thread-safe (a registration answers on a worker).
 */
class OurRelaysStore {
public:
  explicit OurRelaysStore(std::string profile_data_dir);

  /**
   * The kept list; with none kept (a new profile), or one not readable, `org_relay` (named `org_peer_id`, the build's
   * or the config's) then `extras` (config `relay.extra_base_urls`: relay links, MR008), saved. An extra whose link
   * names no PeerId is seeded only when `bare_allowed` (a developer build: checked on first use); one whose fragment
   * is no PeerId never. A newer app's file (a newer `schema_version`) is refused and never written: the seed is used
   * in memory and changes are refused.
   */
  OurRelaysList LoadOrSeed(const std::string& org_relay, const std::string& org_peer_id,
                           const std::vector<std::string>& extras, bool bare_allowed);
  OurRelaysList List() const;
  /**
   * Keeps `list` (bounded: BoundOurRelays); a relay listed before keeps its record unless `list` gives a newer one (by
   * `at_ms`): a list taken before a Record landed never undoes that Record.
   */
  Roe<void> Replace(const OurRelaysList& list);
  /** Keeps what `origin` answered (nothing when it is no longer ours). */
  void Record(const std::string& origin, const RelayRegistrationRecord& record);

private:
  Roe<void> SaveLocked() const;

  std::string profile_data_dir_;
  mutable std::mutex mutex_;
  OurRelaysList list_;
  /** The file is a newer app's (a newer schema_version): refused, and never written (changes are refused). */
  bool newer_file_ = false;
};

/** The file's text for `list` (and back): tests and the store. */
std::string OurRelaysToJson(const OurRelaysList& list);
std::optional<OurRelaysList> OurRelaysFromJson(const std::string& text);

} // namespace pbr
