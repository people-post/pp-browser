#pragma once

#include "lib/net/OurRelaysStore.h"
#include "lib/net/RelayOrigins.h"
#include "lib/net/RelayPins.h"
#include "common/Error.h"
#include "common/Module.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** One relay of ours as Me → Network → My relays shows it. */
struct OurRelayStatus {
  enum class Reach { Unknown, Ok, Failed };
  std::string origin;
  bool preferred = false;
  /** The preferred relay's is the identity's registration there; the others' as they last answered. */
  RelayRegistrationRecord registration;
  /** Its inbox's last poll (this run). */
  Reach reach = Reach::Unknown;
  int64_t reach_at_ms = 0;
  /**
   * It presented another identity than its pin (MR007): not used — not polled, registered on or sent to, left off our
   * card — until the person accepts the new one.
   */
  bool identity_changed = false;
};

struct OurRelaysStatus {
  std::vector<OurRelayStatus> relays;
  /** This device may change the list (it is the main device: it signs the card's list, MR002 / MR006). */
  bool editable = false;
  /**
   * The org's origin, when it is not one of ours and its identity changed (MR007): lockers and the LLM token stop
   * until the person accepts it. Empty otherwise.
   */
  std::string org_identity_changed;
};

/**
 * Changing my relays (multi-relay R3, MR006; docs/contracts/SERVICE_ENDPOINTS.md § My relays). Each change keeps the
 * list (OurRelaysStore), then the services follow it (`apply`: clients, inboxes, the card's relays), our registration
 * goes where it is new, and the card, re-signed with the new list, is announced to our contacts and siblings:
 *
 * - **Add:** the origin must parse (ParseRelayOrigin), be on a public host (IsPublicRelayOrigin) outside developer
 *   builds, the relay must answer (`probe`) and identify itself (`check_identity`, MR007: pinned now; refused when it names no identity
 *   or claims a PeerId another origin holds; a relay seen before with another identity is added only when the person
 *   confirms, `accept_changed`); it is added last and registered on like any other relay of ours.
 * - **Remove:** never the preferred one nor the last; our registration there is revoked first (`leave`: with its push
 *   bindings; best effort — a relay that is down does not keep us on it; none for a relay whose identity changed: we
 *   never sign for it); once the services left it its pin goes (`drop_pin`; kept while the org's services use that
 *   origin), and its inbox cursor goes with the inbox. Undone (kNotApplied): we `rejoin` it.
 * - **Accept new identity:** the relay (or the org's origin) is re-pinned to what it presented (`accept_identity`);
 *   ours is then registered on again (the preferred one as the identity's) and our card re-issued with it.
 * - **Make preferred:** the relay moves first; the identity's registration renews there (`register_preferred`: the
 *   card is newer, so it goes whatever the auto-renew preference — the backend-switch path, which records the relay
 *   the registration was made at), and the old preferred one is registered on as another relay of ours.
 *
 * Only the main device changes the list; a linked device follows the card's (FollowCard). Blocking (HTTP): call from
 * a worker; one change at a time. When the services cannot follow (`apply` fails), the change is undone and refused
 * (kNotApplied). Close (the profile closes) refuses every call after it and waits for the one in progress.
 */
class MyRelaysWorkflow : public Module {
public:
  struct Ports {
    /** Whether the relay at `base_url` answers (GET client-compat). */
    std::function<Roe<void>(const std::string& base_url)> probe;
    /** This device is the account's main one. */
    std::function<bool()> main_device;
    /**
     * The list changed: the services follow it (blocks until they do). False: they did not (the hub's thread did not
     * take it in time, or the profile closes) and never will for this change.
     */
    std::function<bool()> apply;
    /** Our registration on `origin` (another relay of ours) goes, with its push bindings. */
    std::function<Roe<void>(const std::string& origin)> leave;
    /** The identity registers on the (new) preferred relay. */
    std::function<Roe<void>()> register_preferred;
    /** The identity's registration on the preferred relay, as the identity holds it (none: not registered there). */
    std::function<std::optional<RelayRegistrationRecord>()> preferred_registration;
    /** Our other relays are registered on when due (soon, on a worker). */
    std::function<void()> register_others;
    /** Our card goes to our contacts and siblings (it is re-issued with the new list first). */
    std::function<void()> announce;
    /** Asks `base_url` for its identity and pins it when nothing stands against it (RelayPins::CheckForAdd). */
    std::function<AddIdentityCheck(const std::string& base_url)> check_identity;
    /** Pins `base_url` to `peer_id` (the person confirmed a relay seen before with another identity). */
    std::function<Roe<void>(const std::string& base_url, const std::string& peer_id)> repin;
    /** `origin`'s pin goes (Remove…, once the services left it), unless the org's services still use it. */
    std::function<void(const std::string& origin)> drop_pin;
    /** A Remove the services did not follow: `origin`, left already, is ours again (registered on again soon). */
    std::function<void(const std::string& origin)> rejoin;
    /** `origin` presented another identity, not accepted yet. */
    std::function<bool(const std::string& origin)> identity_changed;
    /** `origin` is re-pinned to the identity it presented. */
    std::function<Roe<void>(const std::string& origin)> accept_identity;
    /** Another relay of ours is registered on now (its backoff forgotten). */
    std::function<void(const std::string& origin)> register_now;
    /** The org's origin when it is not ours and its identity changed (OurRelaysStatus::org_identity_changed). */
    std::function<std::string()> org_identity_changed;
    /** The page reads the list again. */
    std::function<void()> changed;
  };

  MyRelaysWorkflow(OurRelaysStore& store, Ports ports);

  /**
   * Adds the relay `typed` names. `developer_build`: a plain `http://` relay, or one not on a public host
   * (IsPublicRelayOrigin), may be added (the lab's); else neither. `accept_changed`: the person confirmed a relay
   * that presents another identity than when it was last seen (kIdentityChangedSinceSeen). The error's message is a
   * code (below); kIdentityTaken is followed by `|` and the host that holds the PeerId.
   */
  Roe<void> Add(const std::string& typed, bool developer_build, bool accept_changed = false);
  /**
   * The person accepts the identity `origin` presented (MR007): one of ours (the main device only), or the org's
   * origin (any device).
   */
  Roe<void> AcceptIdentity(const std::string& origin);
  Roe<void> Remove(const std::string& origin);
  Roe<void> MakePreferred(const std::string& origin);
  /** A linked device: the card's list (preferred first) becomes ours when it differs. */
  void FollowCard(const std::vector<std::string>& card_relays);
  /** An inbox of ours was polled. Any thread. */
  void NotePolled(const std::string& origin, bool ok, int64_t now_ms);
  /** The list as the page shows it; empty once closed. */
  OurRelaysStatus Status() const;
  /**
   * The profile closes: every call from now on is refused (a change with kClosed, Status empty) and no port is called
   * once this returns (it waits for a change or a Status in progress). Call before what the ports touch goes.
   */
  void Close();

  /** The error codes besides RelayListProblemCode's. */
  static constexpr const char* kUnreachable = "relay_unreachable";
  /** A release build takes no relay on this device or a private network. */
  static constexpr const char* kLocal = "relay_origin_local";
  /** The services did not follow the change (it was undone). */
  static constexpr const char* kNotApplied = "relay_list_not_applied";
  /** The profile is closing. */
  static constexpr const char* kClosed = "Messaging is not ready";
  /** Add: the server answers no relay identity (MR007). */
  static constexpr const char* kIdentityUnavailable = "relay_identity_unavailable";
  /** Add: it claims the PeerId of a relay on our list (`|host` follows), or of one we know (kIdentityTakenKnown). */
  static constexpr const char* kIdentityTaken = "relay_identity_taken";
  static constexpr const char* kIdentityTakenKnown = "relay_identity_taken_known";
  /** Add: it presents another identity than when it was last seen: ask the person, then Add with accept_changed. */
  static constexpr const char* kIdentityChangedSinceSeen = "relay_identity_changed_since_seen";
  /** Accept: it presents no other identity. */
  static constexpr const char* kIdentityNotChanged = "relay_identity_not_changed";
  /** "relay_origin_" and the problem's name (not_a_url, not_https, has_path, has_query, too_long, empty). */
  static std::string OriginProblemCode(RelayOriginProblem problem);

private:
  /** A call inside the gate Close waits on; `open` false: closed, the call does nothing. */
  class Entered {
  public:
    explicit Entered(const MyRelaysWorkflow& workflow);
    ~Entered();
    Entered(const Entered&) = delete;
    Entered& operator=(const Entered&) = delete;
    const bool open;

  private:
    const MyRelaysWorkflow& workflow_;
  };

  Roe<void> RequireMainDevice() const;
  /** Add's identity step for `base_url` (pinned, or why not). */
  Roe<void> CheckIdentity(const std::string& base_url, bool accept_changed);
  /** Keeps `next`, the services follow, the registrations and the card go. */
  Roe<void> Commit(const OurRelaysList& next, bool preferred_changed);
  void RegisterPreferred(const std::string& preferred);

  OurRelaysStore& store_;
  Ports ports_;
  std::mutex change_mutex_;
  mutable std::mutex gate_mutex_;
  mutable std::condition_variable gate_cv_;
  mutable int inside_ = 0;
  bool closed_ = false;
  mutable std::mutex reach_mutex_;
  std::map<std::string, std::pair<bool, int64_t>> reach_; // by RelayOriginKey: ok, when
};

} // namespace pbr
