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
#include <set>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What a contact card's (or QR code's) relay entry is to us (MR007 § Pins: conflicts). */
enum class CardRelayCheck {
  /** Our pin for the origin is that PeerId, or we hold no pin for either: sign for it. */
  Usable,
  /** Our pin for the origin is another PeerId, or the PeerId is pinned to another origin: skip that relay. */
  Mismatch,
};

/** What Add finds out about a relay's identity (MR008: what the user sees). */
struct AddIdentityCheck {
  enum class Result {
    /** It answers to the PeerId its link names (pinned now, or already); a bare origin: pinned on first use. */
    Ok,
    /** It answers no identity (unreachable, 503, or an answer that does not check out). */
    Unavailable,
    /** Its PeerId is pinned to another origin (`holder`): the link would front a relay we know at another address. */
    Taken,
    /** Its identity answer names another PeerId than its link, or its origin is expected to answer to another. */
    Mismatch,
  };
  Result result = Result::Ok;
  std::string peer_id;
  std::string holder;
  /** Ok: pinned by this check (an Add that does not go through drops it again). */
  bool pinned_now = false;
};

/**
 * The app's (or pp-node's) relay identities (multi-relay MR007, MR008; docs/contracts/SERVICE_ENDPOINTS.md § Relay
 * identity): the engine over RelayPinStore. A relay address carries its PeerId, so the identity of a relay of ours or
 * of the org's origin is expected (Expect: from its relay link, the build, the config, our own card on a linked
 * device), never learned from the relay: `GET /v1/relay-identity` is only checked (CheckIdentities once per run, a
 * `wrong_audience`, Add): an answer naming another PeerId leaves the relay not used — nothing is signed for it — until
 * a later check finds it answering to its address again (a new key is a new address: the person removes it and adds
 * its new link). Hands every client of a relay of ours (or of the org's origin) its audience (AudienceFor): the
 * expected PeerId. An origin no address names a PeerId for is signed for only where SetFirstUse allows it (a
 * developer build's bare origin; the org's relay in a build that ships no PeerId, with a warning): its first answer is
 * pinned (a fetch that fails pins nothing and is tried again after a backoff, kFirstRetryMs doubling to
 * kMaxOursRetryMs). A contact's card or QR code never writes a pin (CheckCardRelay: their entries are signed for as
 * they name them). `not_registered` / `registration_expired` go to SetOnNotRegistered. Owned by a shared_ptr (the
 * audiences it hands out hold it). Thread-safe; fetches block (HTTP), outside the lock.
 */
class RelayPins : public Module, public std::enable_shared_from_this<RelayPins> {
public:
  static constexpr int64_t kFirstRetryMs = 60 * 1000;
  /** The first-use fetch's backoff cap (AudienceFor). */
  static constexpr int64_t kMaxOursRetryMs = 2 * 60 * 1000;
  static constexpr int64_t kWrongAudienceRefetchMs = 5 * 60 * 1000;

  /** `public_only`: https to a public address only (FetchRelayIdentity). */
  using Fetch = std::function<Roe<RelayIdentity>(const std::string& base_url, bool public_only)>;

  /** `profile_data_dir` holds `relay_pins.json` (empty: in memory). Loaded here. */
  explicit RelayPins(std::string profile_data_dir);

  /** Tests: the identity fetch (default FetchRelayIdentity) and the clock (Unix ms). */
  void SetFetch(Fetch fetch);
  void SetClock(std::function<int64_t()> now_ms);
  /** Which pins stay when the store is full (RelayPinStore::SetKeep). */
  void SetKeep(std::function<bool(const std::string& origin)> keep);
  /**
   * Which origins no address names a PeerId for may be pinned from their first identity answer (MR008 item 4: a
   * developer build's bare origins; the org's relay where the build ships none). None set: no origin.
   */
  void SetFirstUse(std::function<bool(const std::string& origin)> first_use);
  /** A pin was written, changed state or went (any thread; outside the lock). */
  void SetOnChanged(std::function<void(const std::string& origin)> on_changed);
  /** A relay answered a command signed for its pin `not_registered` / `registration_expired` (any thread). */
  void SetOnNotRegistered(std::function<void(const std::string& origin)> on_not_registered);

  /**
   * The audience every client of the relay of ours (or the org's origin) at `origin` signs for (shared; the same object
   * per origin and policy): its expected PeerId. `public_only` (a relay the person added): its identity is asked over
   * https to a public address only, as its clients ask — from then on, whoever asks (CheckIdentities too).
   */
  std::shared_ptr<IRelayAudience> AudienceFor(const std::string& origin, bool public_only = false);

  /** The PeerId pinned for `origin` and in force; none: not pinned, or its identity answer does not match it. */
  std::optional<std::string> PinnedPeerId(const std::string& origin) const;
  /** `origin`'s pin, whatever its state. */
  std::optional<RelayPin> PinFor(const std::string& origin) const;
  /** `origin`'s identity answer named another PeerId than its address (not used). */
  bool IdentityMismatch(const std::string& origin) const;
  /**
   * Our card's relays (MR007): each of `origins` (ours, preferred first) whose pin is in force, with its PeerId; a
   * relay not pinned, or whose identity answer does not match, is left off.
   */
  std::vector<AccountCardRelay> PinnedRelays(const std::vector<std::string>& origins) const;
  /** A contact's card (or QR code) names `{origin, peer_id}`: usable or not by our pins (writes nothing). */
  CardRelayCheck CheckCardRelay(const std::string& origin, const std::string& peer_id) const;

  /**
   * The PeerId to sign for at `origin`: its pin when in force; none pinned and first use allowed: asked now (blocks)
   * and pinned, unless a fetch failed within the backoff. Errors as IRelayAudience::PeerId.
   */
  Roe<std::string> Audience(const std::string& origin, bool public_only = false);
  /**
   * `origin`'s address names `peer_id` (a relay of ours, the org's): expected from now on. Refused
   * (Err::Auth::RelayIdentityTaken) when another origin holds that PeerId — a link or card naming the PeerId of a
   * relay we know at another address would front it.
   */
  Roe<void> Expect(const std::string& origin, const std::string& peer_id);
  /**
   * Asks each origin for its identity, once per run each (again while it does not match, or it did not answer: at
   * start, a list change, the network back or the app in front; blocks), and checks the answer against its pin; an
   * origin with none is pinned where first use is allowed. True when a pin was written or changed state.
   */
  bool CheckIdentities(const std::vector<std::string>& origins);
  /** The relay at `origin` refused a signed command with `code` (IRelayAudience::Refused). */
  void Refused(const std::string& origin, const std::string& code, bool public_only = false);

  /**
   * Add: asks `origin` for its identity now (no backoff; `public_only`: over https to a public address only, outside
   * developer builds) and checks it against `peer_id`, the link's (expected from now on when it matches); `peer_id`
   * empty (a developer build's bare origin): pinned on first use when nothing stands against it. Blocks.
   */
  AddIdentityCheck CheckForAdd(const std::string& origin, const std::string& peer_id, bool public_only = false);
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
  bool FirstUse(const std::string& origin) const;
  Roe<RelayIdentity> FetchIdentity(const std::string& origin, bool public_only) const;
  /** Asks `origin` and checks the answer against the pins; the answer's outcome, or the fetch's error. */
  Roe<PinAnswer> AskAndPresent(const std::string& origin, std::string& presented, bool public_only, bool first_use);
  /** Audience's, `within_backoff` false: asked whatever the backoff (CheckIdentities). */
  Roe<std::string> AudienceAsking(const std::string& origin, bool public_only, bool within_backoff);
  /** CheckIdentities' check of one pinned origin; true when its state changed. */
  bool CheckPinned(const std::string& key, const RelayPin& pin);
  /** A fetch for `key` failed: its next try after the backoff, doubling up to kMaxOursRetryMs. */
  void NoteFetchFailed(const std::string& key);
  /** `asked`, or an audience of `key` was made public-only. */
  bool PublicOnly(const std::string& key, bool asked) const;
  void Changed(const std::string& origin) const;

  RelayPinStore store_;
  /** The hooks (set at setup, read by any thread). */
  mutable std::mutex hooks_mutex_;
  Fetch fetch_;
  std::function<int64_t()> now_ms_;
  std::function<bool(const std::string&)> first_use_;
  std::function<void(const std::string&)> on_changed_;
  std::function<void(const std::string&)> on_not_registered_;
  /** Fetch states and audiences; never held while a hook or a fetch runs. */
  mutable std::mutex mutex_;
  std::map<std::string, FetchState> fetches_;
  std::map<std::string, std::weak_ptr<IRelayAudience>> audiences_;
  /** Origins whose identity is asked over a public address only (AudienceFor's `public_only`), by RelayOriginKey. */
  std::set<std::string> public_only_;
  /** Origins whose identity answered as expected this run (CheckIdentities asks each once), by RelayOriginKey. */
  std::set<std::string> checked_;
};

} // namespace pbr
