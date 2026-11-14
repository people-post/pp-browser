#pragma once

#include "lib/net/OurRelaysStore.h"
#include "lib/net/RelayOrigins.h"
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
};

struct OurRelaysStatus {
  std::vector<OurRelayStatus> relays;
  /** This device may change the list (it is the main device: it signs the card's list, MR002 / MR006). */
  bool editable = false;
};

/**
 * Changing my relays (multi-relay R3, MR006; docs/contracts/SERVICE_ENDPOINTS.md § My relays). Each change keeps the
 * list (OurRelaysStore), then the services follow it (`apply`: clients, inboxes, the card's relays), our registration
 * goes where it is new, and the card, re-signed with the new list, is announced to our contacts and siblings:
 *
 * - **Add:** the origin must parse (ParseRelayOrigin) and the relay must answer (`probe`); it is added last and
 *   registered on like any other relay of ours.
 * - **Remove:** never the preferred one nor the last; our registration there is revoked first (`leave`: with its push
 *   bindings; best effort — a relay that is down does not keep us on it), and its inbox cursor goes with the inbox.
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
    /** The page reads the list again. */
    std::function<void()> changed;
  };

  MyRelaysWorkflow(OurRelaysStore& store, Ports ports);

  /** Adds the relay `typed` names (`allow_http`: developer builds). The error's message is a code (below). */
  Roe<void> Add(const std::string& typed, bool allow_http);
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
  /** The services did not follow the change (it was undone). */
  static constexpr const char* kNotApplied = "relay_list_not_applied";
  /** The profile is closing. */
  static constexpr const char* kClosed = "Messaging is not ready";
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
