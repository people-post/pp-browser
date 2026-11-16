#pragma once

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

/**
 * A relay origin and the PeerId it must answer to (multi-relay MR008: a relay address carries its PeerId, as a node's
 * multiaddr its `/p2p/`): the identity we expect, from the relay link it was added by, the build or the config (the
 * org's), our own card (a linked device) — never from the relay's own answer, but in a developer build for a bare
 * origin (first use).
 */
struct RelayPin {
  /** The relay base URL as relays are compared (RelayOriginKey). */
  std::string origin;
  std::string peer_id;
  int64_t pinned_at_ms = 0;
  /** Another PeerId the origin presented when it was checked: set, it does not match its address (not used). */
  std::string presented_peer_id;
  int64_t presented_at_ms = 0;

  /** Its identity answer named another PeerId than expected: nothing is signed for it. */
  bool Mismatched() const { return !presented_peer_id.empty(); }
  bool operator==(const RelayPin&) const = default;
};

/** What an identity answer, or an expectation, did to the pins. */
enum class PinOutcome {
  /** Written now: an expectation, or a developer build's first use. */
  Pinned,
  /** The PeerId it is pinned to (a mismatch seen before is cleared: it answers to its address again). */
  Same,
  /** Another PeerId than expected: it does not match its address (kept as presented). */
  Mismatch,
  /** A PeerId another origin holds: refused, never pinned. */
  Taken,
  /** Every pin is one the store keeps (SetKeep): not pinned. */
  Full,
  /** No identity is expected for the origin, and it may not be learned by first use. */
  NotExpected,
};

struct PinAnswer {
  PinOutcome outcome = PinOutcome::Pinned;
  /** Taken: the origin that holds the PeerId. */
  std::string holder;
};

/** One `{origin, peer_id}` of our own account card (a linked device takes them as pins). */
struct OwnCardRelay {
  std::string origin;
  std::string peer_id;
};

/**
 * `relay_pins.json` in the profile directory (docs/contracts/DATA_LAYOUT.md): the identities we expect, one PeerId per
 * origin and one origin per PeerId, at most kMaxPins. A pin is written only for a relay of ours or the org's origin:
 * from its address (Expect: a relay link, the build, the config), on a linked device from our own account's card
 * (TakeOwnCard), or, for a bare origin where first use is allowed, from its first identity answer (Present); a
 * contact's card or QR code never writes one. A file of another
 * `schema_version`, or not JSON, is refused and read as empty (logged): the relays are pinned again on first use; a
 * newer one is never overwritten (the pins of this run are kept in memory only).
 * Thread-safe; every change is written at once (atomically).
 */
class RelayPinStore {
public:
  static constexpr size_t kMaxPins = 32;

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
  /** The origin `peer_id` is pinned to (none: no origin holds it). */
  std::optional<std::string> HolderOf(const std::string& peer_id) const;
  std::vector<RelayPin> All() const;

  /**
   * `origin` presented `peer_id` in its own identity answer at `now_ms`: checked against its pin (Same, or Mismatch,
   * kept as presented). No pin: pinned when `first_use` (a developer build's bare origin, the org's without one
   * shipped), else NotExpected.
   */
  PinAnswer Present(const std::string& origin, const std::string& peer_id, int64_t now_ms, bool first_use);
  /**
   * `origin` is expected to answer to `peer_id` (its address names it): pinned, replacing an expectation of another
   * PeerId (and its mismatch); the same one is left as it is (Same). Refused (Taken) when another origin holds it.
   */
  PinAnswer Expect(const std::string& origin, const std::string& peer_id, int64_t now_ms);
  /**
   * A newer card of our own account (a linked device, MR007): each entry becomes a pin, replacing one that disagrees
   * and dropping another origin's pin of the same PeerId. True when a pin changed.
   */
  bool TakeOwnCard(const std::vector<OwnCardRelay>& relays, int64_t now_ms);
  /** Drops `origin`'s pin (Remove…: a relay that claimed another's PeerId stops holding it). */
  void Drop(const std::string& origin);

private:
  std::vector<RelayPin>::iterator FindLocked(const std::string& origin);
  std::vector<RelayPin>::const_iterator FindLocked(const std::string& origin) const;
  std::vector<RelayPin>::const_iterator HolderLocked(const std::string& peer_id) const;
  /** False when the store is full of kept pins (nothing added). */
  bool AddLocked(RelayPin pin);
  void SaveLocked() const;

  std::string profile_data_dir_;
  mutable std::mutex mutex_;
  std::vector<RelayPin> pins_;
  std::function<bool(const std::string&)> keep_;
  /** The file is a newer app's: never written (this run's pins are in memory only). */
  bool newer_file_ = false;
};

/** The file's text for `pins` (and back): tests and the store. Back: nullopt for another schema or not JSON. */
std::string RelayPinsToJson(const std::vector<RelayPin>& pins);
std::optional<std::vector<RelayPin>> RelayPinsFromJson(const std::string& text);

} // namespace pbr
