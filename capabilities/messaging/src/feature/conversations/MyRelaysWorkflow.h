#pragma once

#include "lib/net/OurRelaysStore.h"
#include "lib/net/certificate/RelayOrigins.h"
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

/** One relay of ours as Me → My relays shows it. */
struct OurRelayStatus {
  enum class Reach { Unknown, Ok, Failed };
  std::string origin;
  bool preferred = false;
  /** The preferred relay's is the identity's registration there; the others' as they last answered. */
  RelayRegistrationRecord registration;
  /** Its inbox's last poll (this run). */
  Reach reach = Reach::Unknown;
  int64_t reach_at_ms = 0;
  /** The relay id its address names (MR008; its relay link is `origin#relay:…`); none known: a bare origin. */
  RelayId relay_id;
  /**
   * It does not prove the relay id its address names (SIGNED_FORMATS § 3.4: another relay's certificate, none valid,
   * or one not listing its origin): not used — not polled, registered on or sent to, left off our card — until it
   * proves it again (a new key is a new certificate of the same relay, read at the next check).
   */
  bool identity_mismatch = false;
};

struct OurRelaysStatus {
  std::vector<OurRelayStatus> relays;
  /** This device may change the list (it is the main device: it signs the card's list, MR002 / MR006). */
  bool editable = false;
  /**
   * The org's origin, when it is not one of ours and its identity answer does not match what the build or the config
   * names (MR008): lockers and the LLM token stop. Empty otherwise.
   */
  std::string org_identity_mismatch;
};

/**
 * Changing my relays (multi-relay R3, MR006; docs/contracts/SERVICE_ENDPOINTS.md § My relays). Each change keeps the
 * list (OurRelaysStore), then the services follow it (`apply`: clients, inboxes, the card's relays), our registration
 * goes where it is new, and the card, re-signed with the new list, is announced to our contacts and siblings:
 *
 * - **Add:** a relay link (ParseRelayLink, MR008: `https://host#relay:…`; a bare origin only in developer builds) on a
 *   public host (IsPublicRelayOrigin) outside developer builds; the relay must answer (`probe`) and answer to the
 *   link's relay id (`check_identity`: its certificate checked, expected from now on; refused when it proves no
 *   identity or another relay id, or when that relay is ours at another origin); it is added last and registered on
 *   like any other relay of ours.
 * - **Remove:** never the preferred one nor the last; our registration there is revoked first (`leave`: with its push
 *   bindings; best effort — a relay that is down does not keep us on it; none for a relay that does not match its
 *   address: we never sign for it); once the services left it its pin goes (`drop_pin`; kept while the org's
 *   services use that origin), and its inbox cursor goes with the inbox. Undone (kNotApplied): we `rejoin` it.
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
    /**
     * The identity's registration on the preferred relay, as the identity holds it: Registered, Expired or
     * NotRegistered (none: the profile is closing).
     */
    std::function<std::optional<RelayRegistrationRecord>()> preferred_registration;
    /** Our other relays are registered on when due (soon, on a worker). */
    std::function<void()> register_others;
    /** Our card goes to our contacts and siblings (it is re-issued with the new list first). */
    std::function<void()> announce;
    /**
     * Asks `base_url` for its certificate and checks it against `relay_id`, its link's (expected from now on when
     * nothing stands against it; none: a developer build's bare origin, pinned on first use — RelayPins::CheckForAdd).
     */
    std::function<AddIdentityCheck(const std::string& base_url, const RelayId& relay_id)> check_identity;
    /** `origin`'s pin goes (Remove…, once the services left it), unless the org's services still use it. */
    std::function<void(const std::string& origin)> drop_pin;
    /** A Remove the services did not follow: `origin`, left already, is ours again (registered on again soon). */
    std::function<void(const std::string& origin)> rejoin;
    /** `origin` does not prove the relay id its address names (not used). */
    std::function<bool(const std::string& origin)> identity_mismatch;
    /**
     * The relay id `origin` is expected to prove now, in force (the build's for the org's relay; a developer build's
     * bare origin: learned on first use); none: none.
     */
    std::function<RelayId(const std::string& origin)> relay_id_of;
    /** The org's origin when it is not ours and does not match its address (OurRelaysStatus::org_identity_mismatch). */
    std::function<std::string()> org_identity_mismatch;
    /** The page reads the list again. */
    std::function<void()> changed;
    /** Tests: the clock (Unix ms) a registration's end is read against. */
    std::function<int64_t()> now_ms;
  };

  MyRelaysWorkflow(OurRelaysStore& store, Ports ports);

  /**
   * Adds the relay `typed` names: its relay link (MR008). `developer_build`: a bare origin (no relay ID: checked on
   * first use), a plain `http://` relay, or one not on a public host (IsPublicRelayOrigin), may be added (the lab's);
   * else none of them. The error's message is a code (below); kIdentityTaken is followed by `|` and the host the relay
   * is ours at already.
   */
  Roe<void> Add(const std::string& typed, bool developer_build);
  Roe<void> Remove(const std::string& origin);
  Roe<void> MakePreferred(const std::string& origin);
  /** A linked device: the card's list (preferred first, each with its relay id) becomes ours when it differs. */
  void FollowCard(const std::vector<RelayAddress>& card_relays);
  /**
   * A restore from the account locker (any device: it is the account's own list): `relays` (preferred first) become
   * ours and the services follow; registering and the announce are the restore's own steps. Kept even when the
   * services do not follow now (the next start builds them from it).
   */
  void Restore(const std::vector<RelayAddress>& relays);
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
  /** Add: a release build takes a relay only by its link, which names its relay id (MR008). */
  static constexpr const char* kLinkHasNoId = "relay_link_no_id";
  /** Add: the server answers no relay identity (MR007). */
  static constexpr const char* kIdentityUnavailable = "relay_identity_unavailable";
  /**
   * Add: the link names a relay on our list at another address (`|host` follows), or one we know (kIdentityTakenKnown:
   * the org's): the same relay twice.
   */
  static constexpr const char* kIdentityTaken = "relay_identity_taken";
  static constexpr const char* kIdentityTakenKnown = "relay_identity_taken_known";
  /** Add: the relay proves another relay id than its link names (or none), or its origin is expected to prove another. */
  static constexpr const char* kIdentityMismatch = "relay_identity_mismatch";
  /** "relay_origin_" and the problem's name (not_a_url, not_https, has_path, has_query, too_long, empty, bad_relay_id). */
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
  /**
   * What the page shows for one relay's registration: the preferred one's is the identity's (`preferred`; a failure
   * kept shows while the identity holds none there); another's as it answered, Expired once its end passed, and
   * NotRegistered while the preferred relay holds none (it registers after it).
   */
  static RelayRegistrationRecord ShownRegistration(const RelayRegistrationRecord& kept, bool is_preferred,
                                                   const std::optional<RelayRegistrationRecord>& preferred,
                                                   int64_t now_ms);
  /**
   * Add's identity step for `base_url` named `relay_id` (expected now, or why not); `pinned_now`: this step wrote the
   * pin; `relay_id` becomes the one it proves (a developer build's bare origin: its first certificate's).
   */
  Roe<void> CheckIdentity(const std::string& base_url, RelayId& relay_id, bool& pinned_now);
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
