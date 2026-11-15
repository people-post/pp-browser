#pragma once

#include "lib/net/RelayAudience.h"
#include "lib/net/RelayIdentity.h"
#include "lib/net/RelayPinStore.h"
#include "lib/people/AccountCard.h"
#include "common/Module.h"

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What a contact card's relay entry is to us (MR007 § Pins: conflicts). */
enum class CardRelayCheck {
  /** Our pin for the origin is that PeerId, or we hold no pin for either: sign for it. */
  Usable,
  /** Our pin for the origin is another PeerId, or the PeerId is pinned to another origin: skip that relay. */
  Mismatch,
};

/** What Add finds out about a relay's identity (MR007: what the user sees). */
struct AddIdentityCheck {
  enum class Result {
    /** Pinned now, or already pinned to the PeerId it presents. */
    Ok,
    /** It answers no identity (unreachable, 503, or an answer that does not check out). */
    Unavailable,
    /** It presents a PeerId pinned to another origin (`holder`). */
    Taken,
    /** Pinned before (the org's origin, or a relay removed since) with another PeerId: Add asks the person, then Repin. */
    ChangedSinceSeen,
  };
  Result result = Result::Ok;
  std::string peer_id;
  std::string holder;
  /** Ok: pinned by this check (an Add that does not go through drops it again). */
  bool pinned_now = false;
};

/**
 * The app's (or pp-node's) relay pins (multi-relay MR007; docs/contracts/SERVICE_ENDPOINTS.md § Relay identity): the
 * engine over RelayPinStore. Hands every client of a relay of ours (or of the org's origin) its audience (AudienceFor):
 * the pinned PeerId, asked of the origin itself and pinned before the first command is signed for it (a fetch that
 * fails pins nothing and is tried again after a backoff, kFirstRetryMs doubling to kMaxRetryMs). A relay not ours (a
 * QR locator's, its card not kept yet) is never pinned (ForeignAudienceFor): what it presents is kept in memory for
 * its sends only, so no one else's relay can take a PeerId or an origin from ours. A `wrong_audience` asks the identity again (at
 * most once per origin per kWrongAudienceRefetchMs): another PeerId than the pin is an *identity changed* — nothing
 * is signed for that relay until the person accepts it (Accept); the same PeerId is a relay misbehaving (logged).
 * `not_registered` / `registration_expired` go to SetOnNotRegistered. Owned by a shared_ptr (the audiences it hands out
 * hold it). Thread-safe; fetches block (HTTP), outside the lock.
 */
class RelayPins : public Module, public std::enable_shared_from_this<RelayPins> {
public:
  static constexpr int64_t kFirstRetryMs = 60 * 1000;
  static constexpr int64_t kMaxRetryMs = 30 * 60 * 1000;
  static constexpr int64_t kWrongAudienceRefetchMs = 5 * 60 * 1000;
  /** Relays not ours whose presented PeerId is kept in memory (ForeignAudienceFor). */
  static constexpr size_t kMaxForeign = 64;

  /** `public_only`: the origin is not ours (FetchRelayIdentity). */
  using Fetch = std::function<Roe<RelayIdentity>(const std::string& base_url, bool public_only)>;

  /** `profile_data_dir` holds `relay_pins.json` (empty: in memory). Loaded here. */
  explicit RelayPins(std::string profile_data_dir);

  /** Tests: the identity fetch (default FetchRelayIdentity) and the clock (Unix ms). */
  void SetFetch(Fetch fetch);
  void SetClock(std::function<int64_t()> now_ms);
  /** Which pins stay when the store is full (RelayPinStore::SetKeep). */
  void SetKeep(std::function<bool(const std::string& origin)> keep);
  /** A pin was written, changed state or went (any thread; outside the lock). */
  void SetOnChanged(std::function<void(const std::string& origin)> on_changed);
  /** A relay answered a command signed for its pin `not_registered` / `registration_expired` (any thread). */
  void SetOnNotRegistered(std::function<void(const std::string& origin)> on_not_registered);

  /**
   * The audience every client of the relay of ours (or the org's origin) at `origin` signs for (shared; the same object
   * per origin and policy); its first answer is pinned. `public_only`: its identity is asked over https to a public
   * address only, as its clients ask.
   */
  std::shared_ptr<IRelayAudience> AudienceFor(const std::string& origin, bool public_only = false);
  /**
   * The audience of a relay not ours that no card names a PeerId for (a QR locator's): the PeerId it presents, asked
   * over https to a public address only when `public_only`, kept in memory (at most kMaxForeign relays) and never
   * pinned. One that presents a PeerId pinned to another origin is refused (Err::Auth::RelayIdentityTaken); an origin
   * we pin (a locator naming a relay of ours) signs for its pin. Never makes another origin's card entry Mismatch.
   */
  std::shared_ptr<IRelayAudience> ForeignAudienceFor(const std::string& origin, bool public_only);

  /** The PeerId pinned for `origin` and in force (not changed); none: not pinned, or its identity changed. */
  std::optional<std::string> PinnedPeerId(const std::string& origin) const;
  /** `origin`'s pin, whatever its state. */
  std::optional<RelayPin> PinFor(const std::string& origin) const;
  /** `origin` presented another identity than its pin, not accepted yet. */
  bool IdentityChanged(const std::string& origin) const;
  /**
   * Our card's relays (MR007): each of `origins` (ours, preferred first) whose pin is in force, with its PeerId; a
   * relay not pinned yet, or whose identity changed, is left off.
   */
  std::vector<AccountCardRelay> PinnedRelays(const std::vector<std::string>& origins) const;
  /** A contact's card names `{origin, peer_id}`: usable or not by our pins (writes nothing). */
  CardRelayCheck CheckCardRelay(const std::string& origin, const std::string& peer_id) const;

  /**
   * The PeerId to sign for at `origin`: its pin when in force; none pinned yet: asked now (blocks) and pinned, unless a
   * fetch failed within the backoff. Errors as IRelayAudience::PeerId.
   */
  Roe<std::string> Audience(const std::string& origin, bool public_only = false);
  /** Pins each origin not pinned yet (asked in turn; blocks). True when a pin was written. */
  bool EnsurePinned(const std::vector<std::string>& origins);
  /** The relay at `origin` refused a signed command with `code` (IRelayAudience::Refused). */
  void Refused(const std::string& origin, const std::string& code, bool public_only = false);
  /** ForeignAudienceFor's: the PeerId to sign for at a relay not ours (asked when none is kept; blocks). */
  Roe<std::string> ForeignAudience(const std::string& origin, bool public_only);
  /** ForeignAudienceFor's: `wrong_audience` forgets what it presented (asked again, at most every kWrongAudienceRefetchMs). */
  void ForeignRefused(const std::string& origin, const std::string& code);

  /** Add: asks `origin` for its identity now (no backoff) and pins it when nothing stands against it. Blocks. */
  AddIdentityCheck CheckForAdd(const std::string& origin);
  /** The person accepts the identity `origin` presented: re-pinned to it. An error when there is none to accept. */
  Roe<void> Accept(const std::string& origin);
  /** Pins `origin` to `peer_id` whatever it was pinned to (Add's confirm); an error when another origin holds it. */
  Roe<void> Repin(const std::string& origin, const std::string& peer_id);
  /** A newer card of our own account on a linked device: its entries become pins (RelayPinStore::TakeOwnCard). */
  void TakeOwnCard(const std::vector<OwnCardRelay>& relays);
  /** Remove…: `origin`'s pin goes. */
  void Drop(const std::string& origin);

private:
  /** Per origin: when a fetch may run again. */
  struct FetchState {
    int64_t next_try_ms = 0;
    int64_t backoff_ms = 0;
    int64_t wrong_audience_at_ms = 0;
  };

  int64_t Now() const;
  /** Asks `origin` and applies the answer to the pins; the answer's outcome, or the fetch's error. */
  Roe<PinAnswer> AskAndPresent(const std::string& origin, std::string& presented, bool public_only);
  void NoteFetchFailed(const std::string& key);
  void Changed(const std::string& origin) const;

  RelayPinStore store_;
  /** The hooks (set at setup, read by any thread). */
  mutable std::mutex hooks_mutex_;
  Fetch fetch_;
  std::function<int64_t()> now_ms_;
  std::function<void(const std::string&)> on_changed_;
  std::function<void(const std::string&)> on_not_registered_;
  /** Fetch states and audiences; never held while a hook or a fetch runs. */
  mutable std::mutex mutex_;
  std::map<std::string, FetchState> fetches_;
  std::map<std::string, std::weak_ptr<IRelayAudience>> audiences_;
  /** Relays not ours: the PeerId each presented last (memory only, never pinned), by RelayOriginKey. */
  std::map<std::string, std::string> foreign_;
};

} // namespace pbr
