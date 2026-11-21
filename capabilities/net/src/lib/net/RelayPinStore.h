#pragma once

#include "lib/net/RelayCertificate.h"

#include "foundation/crypto/RelayId.h"

#include "common/Error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Why an origin does not match its address (SIGNED_FORMATS § 3.4 step 4: not used, logged, left off our card). */
enum class RelayMismatch : uint8_t {
  None = 0,
  /** It serves the certificate (or names the relay id) of another relay. */
  OtherRelay = 1,
  /** It serves no certificate that verifies against the relay id its address names. */
  NoValidCertificate = 2,
  /** Its certificate verifies but does not list the origin it was asked at (a relay fronting another). */
  OriginNotListed = 3,
};

/** "other_relay", "no_valid_certificate", "origin_not_listed" ("" for None): the file's and the log's word. */
const char* RelayMismatchName(RelayMismatch mismatch);
RelayMismatch RelayMismatchFromName(const std::string& name);

/**
 * A relay origin and the relay id it must prove (multi-relay MR008 as SIGNED_FORMATS § 3.3 amends it: a relay
 * address is its origin and its relay id): from the relay link it was added by, the build or the config (the org's),
 * our own card (a linked device) — never from the relay's own answer, but in a developer build for a bare origin, or
 * the org's relay in a build that names none (first use).
 */
struct RelayPin {
  /** The relay base URL as relays are compared (RelayOriginKey). */
  std::string origin;
  RelayId relay_id;
  int64_t pinned_at_ms = 0;
  /** Set: when it was last checked it did not prove `relay_id` at this origin (not used). */
  RelayMismatch mismatch = RelayMismatch::None;
  /** OtherRelay: the relay id it named instead (none when it named none). */
  RelayId presented_relay_id;
  int64_t mismatch_at_ms = 0;

  bool Mismatched() const { return mismatch != RelayMismatch::None; }
  bool operator==(const RelayPin&) const = default;
};

/** What an expectation did to the pins. */
enum class PinOutcome {
  /** Written now (new, or replacing an expectation of another relay id and its mismatch). */
  Pinned,
  /** The relay id it is pinned to already (a mismatch seen stays until a check finds the origin proves it again). */
  Same,
  /** Every pin is one the store keeps (SetKeep): not pinned. */
  Full,
};

/** A certificate held for a relay id: the highest valid one seen from any channel. */
struct HeldRelayCertificate {
  RelayCertificate certificate;
  /** When it was kept (Unix ms): the oldest of the relays no pin names goes first when the store is full. */
  int64_t kept_at_ms = 0;
};

/**
 * `relay_pins.json` in the profile directory (docs/contracts/DATA_LAYOUT.md), `schema_version` 3: the relay ids we
 * expect per origin (at most kMaxPins; one relay id per origin, a relay id at up to 4 origins as its certificate
 * lists them) and, per relay id, the highest valid certificate seen (at most kMaxCertificates: ours, the org's and the
 * contacts' relays, which prove themselves and never become ours). A pin is written only for a relay of ours or the
 * org's origin: from its address (Expect), from our own account's card on a linked device (TakeOwnCard), or by first
 * use where allowed. A file of another `schema_version`, or not JSON, is refused and read as empty (logged): the
 * relays are expected again from their addresses; a newer one is never overwritten (this run's pins are in memory
 * only). Each certificate is verified again when the file is read (a certificate that does not is dropped).
 * Thread-safe; every change is written at once (atomically).
 */
class RelayPinStore {
public:
  static constexpr size_t kMaxPins = 32;
  static constexpr size_t kMaxCertificates = 32;

  /** `profile_data_dir` empty: in memory only (tests). */
  explicit RelayPinStore(std::string profile_data_dir);

  /** Reads the file (none: no pins). */
  void Load();
  /**
   * Which pins must stay when the store is full: ours and the org's origins. The oldest pin it does not keep (a relay
   * removed from our list, or one an Add that did not go through pinned) goes first.
   */
  void SetKeep(std::function<bool(const std::string& origin)> keep);

  std::optional<RelayPin> PinFor(const std::string& origin) const;
  /** The origins pinned to `relay_id`, in pin order. */
  std::vector<std::string> OriginsOf(const RelayId& relay_id) const;
  std::vector<RelayPin> All() const;

  /**
   * `origin` is expected to prove `relay_id` (its address names it): pinned, replacing an expectation of another relay
   * id (and its mismatch); the same one is left as it is (Same).
   */
  PinOutcome Expect(const std::string& origin, const RelayId& relay_id, int64_t now_ms);
  /**
   * What a check of `origin` found: `mismatch` None clears a mismatch seen before (true when it was set), anything
   * else records it (true when it changed: a mismatch already recorded so is not new). No pin: nothing.
   */
  bool NoteCheck(const std::string& origin, RelayMismatch mismatch, const RelayId& presented, int64_t now_ms);
  /**
   * A newer card of our own account (a linked device, MR007): each entry becomes a pin, replacing one that disagrees.
   * True when a pin changed.
   */
  bool TakeOwnCard(const std::vector<RelayAddress>& relays, int64_t now_ms);
  /** Drops `origin`'s pin. */
  void Drop(const std::string& origin);

  /** The certificate held for `relay_id`. */
  std::optional<RelayCertificate> CertificateFor(const RelayId& relay_id) const;
  /**
   * `certificate` (valid, checked by the caller against its relay id) is offered: kept when none is held for its relay
   * id or its sequence is higher (Adopted, written); Same, Lower and Conflict keep the one held.
   */
  RelayCertificateOffer Offer(const RelayCertificate& certificate, int64_t now_ms);

private:
  std::vector<RelayPin>::iterator FindLocked(const std::string& origin);
  std::vector<RelayPin>::const_iterator FindLocked(const std::string& origin) const;
  /** False when the store is full of kept pins (nothing added). */
  bool AddLocked(RelayPin pin);
  void SaveLocked() const;

  std::string profile_data_dir_;
  mutable std::mutex mutex_;
  std::vector<RelayPin> pins_;
  std::vector<HeldRelayCertificate> certificates_;
  std::function<bool(const std::string&)> keep_;
  /** The file is a newer app's: never written (this run's pins are in memory only). */
  bool newer_file_ = false;
};

/** The file's contents (tests and the store). */
struct RelayPinsFile {
  std::vector<RelayPin> pins;
  std::vector<HeldRelayCertificate> certificates;
};

/**
 * The file's text for `file` (and back): tests and the store. Back: nullopt for another schema or not JSON; a row that
 * does not read (a relay id not of the form, a certificate that does not verify against its relay id) is dropped.
 */
std::string RelayPinsToJson(const RelayPinsFile& file);
std::optional<RelayPinsFile> RelayPinsFromJson(const std::string& text);

} // namespace pbr
