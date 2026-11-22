#pragma once

#include "lib/net/RelayAudience.h"
#include "lib/net/RelayCertificate.h"
#include "lib/net/RelayIdentity.h"
#include "lib/net/RelayPinStore.h"
#include "common/Module.h"

#include "foundation/crypto/RelayId.h"

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What a contact card's (or QR code's) relay entry is to us (SERVICE_ENDPOINTS § Pins: conflicts). */
enum class CardRelayCheck {
  /** Our pin for the origin is that relay id, or we hold none and no certificate says otherwise: usable. */
  Usable,
  /** Our pin for the origin is another relay id, or that relay's certificate does not list the origin: skipped. */
  Mismatch,
};

/** What Add finds out about a relay's identity (MR008: what the user sees). */
struct AddIdentityCheck {
  enum class Result {
    /** It proves the relay id its link names (pinned now, or already); a bare origin: pinned on first use. */
    Ok,
    /** It answers no identity (unreachable, 503, an answer from before the certificate, or one this app cannot read). */
    Unavailable,
    /** That relay is one of ours (or the org's) already, at another origin (`holder`). */
    Taken,
    /** It proves another relay id than its link names, none, or one that does not list its origin. */
    Mismatch,
  };
  Result result = Result::Ok;
  RelayId relay_id;
  std::string holder;
  /** Ok: pinned by this check (an Add that does not go through drops it again). */
  bool pinned_now = false;
};

/**
 * The app's (or pp-node's) relay identities (release-compatibility SIGNED_FORMATS § 3.4; docs/contracts/
 * SERVICE_ENDPOINTS.md § Relay identity, § Pins): the engine over RelayPinStore. A relay address is its origin and
 * its **relay id** (the hash of its founding operator set); the relay id of a relay of ours or of the org's origin is
 * expected (Expect: from its relay link, the build, the config, our own card on a linked device), never learned from
 * the relay, but where first use is allowed. What the relay proves is its **certificate**: signed by its operator set,
 * it names the transport key whose PeerId is the audience of every signed call, and the origins where it answers. The
 * certificate comes from `GET /v1/relay-identity` (CheckIdentities once per run, a `wrong_audience`, Add, a first
 * signed call with none held), `client-compat` and the build (OfferCertificate); per relay id the highest valid one is
 * kept and a lower one never accepted. An origin that serves the certificate of another relay, none valid, or one not
 * listing it is not used — nothing is signed for it — until a later check finds it proving its address again. A
 * contact's relay (ContactAudience) is checked the same way, its certificate cached under its relay id; it never
 * writes a pin. `not_registered` / `registration_expired` go to SetOnNotRegistered, `client_upgrade_required` to
 * SetOnUpgradeRequired. Owned by a shared_ptr (the audiences it hands out hold it). Thread-safe; fetches block
 * (HTTP), outside the lock.
 */
class RelayPins : public Module, public std::enable_shared_from_this<RelayPins> {
public:
  static constexpr int64_t kFirstRetryMs = 60 * 1000;
  /** A failed fetch's backoff cap (Audience). */
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
   * Which origins no address names a relay id for may be pinned from their first valid certificate (MR008 item 4: a
   * developer build's bare origins; the org's relay where the build names none). None set: no origin.
   */
  void SetFirstUse(std::function<bool(const std::string& origin)> first_use);
  /** A pin or a certificate changed (any thread; outside the lock). */
  void SetOnChanged(std::function<void(const std::string& origin)> on_changed);
  /** A relay answered a command signed for it `not_registered` / `registration_expired` (any thread). */
  void SetOnNotRegistered(std::function<void(const std::string& origin)> on_not_registered);
  /** A relay answered a command `426 client_upgrade_required`: this app is too old for it (any thread; each time). */
  void SetOnUpgradeRequired(std::function<void(const std::string& origin)> on_upgrade_required);
  /**
   * A relay answered a command `400 relay_upgrade_required` (or an older relay's `unsupported_sign_version`): that
   * relay is older than this app (U11; any thread; each time).
   */
  void SetOnRelayOutdated(std::function<void(const std::string& origin)> on_relay_outdated);

  /**
   * The audience every client of the relay of ours (or the org's origin) at `origin` signs for (shared; the same object
   * per origin and policy): the PeerId of its held certificate's transport key. `public_only` (a relay the person
   * added): its identity is asked over https to a public address only, as its clients ask — from then on, whoever asks
   * (CheckIdentities too).
   */
  std::shared_ptr<IRelayAudience> AudienceFor(const std::string& origin, bool public_only = false);
  /**
   * The audience of a contact's relay `{origin, relay_id}` (their card's or QR code's entry, the inviter's word): the
   * PeerId its certificate proves, fetched from `origin` (public addresses only unless `public_only` is false) when
   * none held lists the origin. Writes no pin.
   */
  std::shared_ptr<IRelayAudience> ContactAudience(const std::string& origin, const RelayId& relay_id,
                                                  bool public_only = true);

  /** The relay id pinned for `origin` and in force; none: not pinned, or it does not prove it there. */
  std::optional<RelayId> PinnedRelayId(const std::string& origin) const;
  /** `origin`'s pin, whatever its state. */
  std::optional<RelayPin> PinFor(const std::string& origin) const;
  /** `origin` does not prove the relay id its address names (not used). */
  bool IdentityMismatch(const std::string& origin) const;
  /** The certificate held for `relay_id` (the highest valid one seen). */
  std::optional<RelayCertificate> CertificateFor(const RelayId& relay_id) const;
  /**
   * The audience PeerId at `origin` as held now, without asking: its pin in force, and its relay's certificate held,
   * listing `origin` and not expired. None otherwise (Audience asks).
   */
  std::optional<std::string> HeldAudience(const std::string& origin) const;
  /** Our card's relays (MR007): each of `origins` (ours, preferred first) whose pin is in force, with its relay id. */
  std::vector<RelayAddress> PinnedRelays(const std::vector<std::string>& origins) const;
  /** A contact's card (or QR code) names `{origin, relay_id}`: usable or not by our pins (writes nothing). */
  CardRelayCheck CheckCardRelay(const std::string& origin, const RelayId& relay_id) const;

  /**
   * The PeerId to sign for at `origin` (blocks when it asks): its held certificate's when that lists the origin;
   * otherwise its certificate is asked now (unless a fetch failed within the backoff) and checked against the pinned
   * relay id — or, none pinned and first use allowed, pinned from a valid certificate that lists the origin. Errors:
   * Err::Auth::RelayIdentityMismatch (it does not prove its address), Err::Network::RelayIdentityUnavailable (no
   * relay id named, unreachable, no answer this app reads).
   */
  Roe<std::string> Audience(const std::string& origin, bool public_only = false);
  /** ContactAudience's PeerId: as Audience, for the contact's `relay_id`, writing no pin. */
  Roe<std::string> ContactPeerId(const std::string& origin, const RelayId& relay_id, bool public_only = true);
  /** `origin`'s address names `relay_id` (a relay of ours, the org's): expected from now on. */
  Roe<void> Expect(const std::string& origin, const RelayId& relay_id);
  /**
   * A certificate of `relay_id` from a channel other than the relay's own answer (`client-compat`, the build's floor;
   * `channel` names it for the log): kept when it verifies against `relay_id` and is higher than the one held. The
   * offer's outcome, or why it was refused (RelayCertificateCheck::ToError).
   */
  Roe<RelayCertificateOffer> OfferCertificate(const RelayId& relay_id, const std::string& certificate_b64,
                                              std::string_view channel);
  /**
   * Asks each origin for its certificate, once per run each (again while it does not match, or it did not answer: at
   * start, a list change, the network back or the app in front; blocks), and checks it against its pin; an origin
   * with none is pinned where first use is allowed. True when a pin or a certificate changed.
   */
  bool CheckIdentities(const std::vector<std::string>& origins);
  /** The relay at `origin` refused a signed command with `code` (IRelayAudience::Refused). */
  void Refused(const std::string& origin, const std::string& code, bool public_only = false);

  /**
   * Add: asks `origin` for its certificate now (no backoff; `public_only`: over https to a public address only,
   * outside developer builds) and checks it against `relay_id`, the link's (expected from now on when it proves it);
   * `relay_id` empty (a developer build's bare origin): pinned on first use when nothing stands against it. Blocks.
   */
  AddIdentityCheck CheckForAdd(const std::string& origin, const RelayId& relay_id, bool public_only = false);
  /** A newer card of our own account on a linked device: its entries become pins (RelayPinStore::TakeOwnCard). */
  void TakeOwnCard(const std::vector<RelayAddress>& relays);
  /** Remove…: `origin`'s pin goes (its relay's certificate stays cached: it proves itself). */
  void Drop(const std::string& origin);

private:
  /** Per origin (and per contact relay): when a fetch may run again. */
  struct FetchState {
    int64_t next_try_ms = 0;
    int64_t backoff_ms = 0;
    int64_t wrong_audience_at_ms = 0;
  };
  /** What asking an origin for its certificate found. */
  struct Asked {
    enum class Outcome {
      /** It serves a valid certificate of the relay id asked for (or, none asked, its own) that lists the origin. */
      Proved,
      /** It does not prove it (`mismatch`). */
      Mismatch,
      /** No answer read: unreachable, an error status, a newer certificate than this app reads. */
      Unavailable,
    };
    Outcome outcome = Outcome::Unavailable;
    RelayMismatch mismatch = RelayMismatch::None;
    /** Mismatch (OtherRelay): the relay id it named; Proved: the relay id proved. */
    RelayId presented;
    /** Proved: the certificate held for it after the offer (the highest). */
    std::optional<RelayCertificate> held;
    /** The certificate it served was kept (a higher one than held, or the first). */
    bool adopted = false;
    /** Unavailable: why. */
    std::optional<Error> error;
  };

  int64_t Now() const;
  bool FirstUse(const std::string& origin) const;
  Roe<RelayIdentity> FetchIdentity(const std::string& origin, bool public_only) const;
  /** Asks `origin` and checks its certificate against `expected` (empty: any relay id); keeps it when higher. */
  Asked Ask(const std::string& origin, const RelayId& expected, bool public_only);
  /** The certificate held for `relay_id` when it lists `origin` and has not expired. */
  std::optional<RelayCertificate> Usable(const std::string& origin, const RelayId& relay_id) const;
  /** Audience's, `within_backoff` false: asked whatever the backoff (CheckIdentities). */
  Roe<std::string> AudienceAsking(const std::string& origin, bool public_only, bool within_backoff);
  /** CheckIdentities' check of one pinned origin; true when its state changed. */
  bool CheckPinned(const std::string& key, const RelayPin& pin);
  /** Records what a check of a pinned origin found; true when its state changed. */
  bool Note(const std::string& key, const RelayPin& pin, const Asked& asked);
  /** A fetch for `slot` failed: its next try after the backoff, doubling up to kMaxOursRetryMs. */
  void NoteFetchFailed(const std::string& slot);
  /** A fetch for `slot` may run now (not within the backoff of a failed one). */
  bool MayFetch(const std::string& slot) const;
  /** `asked`, or an audience of `key` was made public-only. */
  bool PublicOnly(const std::string& key, bool asked) const;
  /** A `wrong_audience` for `slot` may ask again now (at most once per kWrongAudienceRefetchMs). */
  bool WrongAudienceMayAsk(const std::string& slot);
  void Changed(const std::string& origin) const;
  /** Changed for every origin pinned to `relay_id`. */
  void ChangedRelay(const RelayId& relay_id) const;

  friend class ContactRelayAudience;
  void RefusedContact(const std::string& origin, const RelayId& relay_id, const std::string& code, bool public_only);

  RelayPinStore store_;
  /** The hooks (set at setup, read by any thread). */
  mutable std::mutex hooks_mutex_;
  Fetch fetch_;
  std::function<int64_t()> now_ms_;
  std::function<bool(const std::string&)> first_use_;
  std::function<void(const std::string&)> on_changed_;
  std::function<void(const std::string&)> on_not_registered_;
  std::function<void(const std::string&)> on_upgrade_required_;
  std::function<void(const std::string&)> on_relay_outdated_;
  /** Fetch states and audiences; never held while a hook or a fetch runs. */
  mutable std::mutex mutex_;
  std::map<std::string, FetchState> fetches_;
  std::map<std::string, std::weak_ptr<IRelayAudience>> audiences_;
  /** Origins whose identity is asked over a public address only (AudienceFor's `public_only`), by RelayOriginKey. */
  std::set<std::string> public_only_;
  /** Origins that proved their address this run (CheckIdentities asks each once), by RelayOriginKey. */
  std::set<std::string> checked_;
};

} // namespace pbr
