#pragma once

#include "common/Error.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What a relay of ours last answered our registration with (kept: Me → Network → My relays shows it after a restart). */
struct RelayRegistrationRecord {
  enum class State { Unknown, Registered, Failed };
  State state = State::Unknown;
  /** Failed: what went wrong, as the error said it (one line). */
  std::string reason;
  /** Registered: until when (ISO-8601, the relay's `expires_at`). */
  std::string expires_at;
  /** Registered: the `issued_at` of the card it took (a newer card registers there again). */
  int64_t card_issued_at = 0;
  /** When it answered (Unix ms). */
  int64_t at_ms = 0;

  bool operator==(const RelayRegistrationRecord&) const = default;
};

/** One relay of ours: its base URL and its last registration. */
struct OurRelayEntry {
  std::string origin;
  RelayRegistrationRecord registration;

  bool operator==(const OurRelayEntry&) const = default;
};

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

/** `list` with `base_url` added last (a relay of ours already, or a fifth: refused). */
Roe<OurRelaysList> WithRelayAdded(const OurRelaysList& list, const std::string& base_url);
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

  /** The kept list; with none kept (a new profile, or one from before R3), `org_relay` then `extras`, saved. */
  OurRelaysList LoadOrSeed(const std::string& org_relay, const std::vector<std::string>& extras);
  OurRelaysList List() const;
  /** Keeps `list` (bounded: BoundOurRelays); a relay listed before keeps its record unless `list` gives one. */
  Roe<void> Replace(const OurRelaysList& list);
  /** Keeps what `origin` answered (nothing when it is no longer ours). */
  void Record(const std::string& origin, const RelayRegistrationRecord& record);

private:
  Roe<void> SaveLocked() const;

  std::string profile_data_dir_;
  mutable std::mutex mutex_;
  OurRelaysList list_;
};

/** The file's text for `list` (and back): tests and the store. */
std::string OurRelaysToJson(const OurRelaysList& list);
std::optional<OurRelaysList> OurRelaysFromJson(const std::string& text);

} // namespace pbr
