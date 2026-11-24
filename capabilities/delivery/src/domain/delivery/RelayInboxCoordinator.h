#pragma once

#include "lib/delivery/RelayClients.h"
#include "lib/net/RelayInboxCursor.h"
#include "common/Module.h"
#include "common/chat/RelayEnvelope.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** One of our relay inboxes (multi-relay R2): the relay's origin and its client. */
struct RelayInbox {
  std::string origin;
  std::shared_ptr<IRelayClient> client;
};

/**
 * Polls every relay inbox of ours (multi-relay R2; docs/contracts/SERVICE_ENDPOINTS.md § Several relays): the
 * preferred relay at every sync (the app's pace, BackgroundSyncScheduler), the others at every
 * kOtherRelaysEverySyncs-th — and all of them on a forced sync (a push or a wake: any relay may hold the mail that
 * woke us). Another relay is polled only once we are registered there. Each inbox keeps its own cursor (persisted,
 * RelayInboxCursors, by relay: a relay made preferred keeps its cursor, one no longer ours loses it) and one poll at a time; a batch is handed to `take` on the ingest lane, then acked on the relay
 * it came from. A message that arrives through two relays is taken twice: the receive pipeline drops the second by
 * its id. A cursor counts in one mailbox: the registration we held there (Ports::mailbox, kept beside the cursor). A
 * fresh registration is a new mailbox numbered from the start, so a cursor kept for another one, or while it was not
 * known, is dropped and that inbox is read from its start (what was taken already is dropped as seen); cursors from a
 * file that noted no mailbox at all (an older app's) are read again once, in two phases per relay (RelayInboxReread):
 * first on from the kept cursor exactly as before, until an empty page; then from the inbox's start, skipping what the
 * relay stored before the file's last write (kRereadFloorSlackMs), until an empty page. A relay that refuses the poll for good (IsPermanentRelayRefusal: `426 client_upgrade_required`, `400
 * relay_upgrade_required` / `unsupported_sign_version`, `401 wrong_audience`; release-compatibility T14) is not polled
 * again, forced or not, for kRefusedFirstMs, doubling to kRefusedMaxMs while it keeps refusing; an answer ends the
 * back-off. Thread-safe; the polls' HTTP runs on the `poll` lane.
 */
class RelayInboxCoordinator : public Module {
public:
  static constexpr int kOtherRelaysEverySyncs = 4;
  /** After a permanent refusal (T14): the first wait before that relay is polled again, and the longest. */
  static constexpr int64_t kRefusedFirstMs = 60 * 1000;
  static constexpr int64_t kRefusedMaxMs = 30 * 60 * 1000;
  /**
   * Phase 2 of an older app's file's re-read skips the records the relay stored before the file's last write less this
   * (clock skew between this device and the relay).
   */
  static constexpr int64_t kRereadFloorSlackMs = 10 * 60 * 1000;
  /** Phase 2 skips the records phase 1 took, by message id: at most this many noted per relay (in memory only). */
  static constexpr size_t kRereadTakenMax = 4096;

  using Post = std::function<void(std::function<void()>)>;

  /** A poll's messages, as one relay gave them. */
  struct Batch {
    std::string origin;
    bool preferred = false;
    std::vector<RelayEnvelope> messages;
  };

  struct Ports {
    /** Where a poll runs (blocking HTTP): the background worker. */
    Post poll;
    /** Where a batch is taken in: the critical worker. */
    Post ingest;
    /** Where an ack runs (HTTP): the normal worker. */
    Post ack;
    /** Takes a batch in for `account_id` (the receive pipeline), on the ingest lane. */
    std::function<void(const Batch& batch, const std::string& account_id)> take;
    /** A poll's outcome (null: it answered), on the poll lane. */
    std::function<void(const std::string& origin, bool preferred, const Error* failure)> polled;
    /** Whether another relay may be polled now (we are registered there); none: always. */
    std::function<bool(const std::string& origin)> ready;
    /** Monotonic ms (the refusal back-off); none: the steady clock. */
    std::function<int64_t()> now_ms;
    /**
     * The registration we hold at that relay now (its `registration_id`: new for a fresh registration — a new mailbox
     * —, the same across renewals); empty: unknown, the cursor kept is used. None: always unknown.
     */
    std::function<std::string(const std::string& origin, bool preferred)> mailbox;
    /** When the cursor file in `profile_data_dir` was last written (Unix ms); none: RelayInboxCursorFileWrittenMs. */
    std::function<std::optional<int64_t>(const std::string& profile_data_dir)> cursors_written_ms;
  };

  explicit RelayInboxCoordinator(Ports ports);

  /**
   * Ours, preferred first; `profile_data_dir` keeps the cursors (empty: in memory only). An inbox whose relay, client
   * and place did not change is kept (a poll under way there goes on, its cursor kept); another's poll under way ends.
   * The cursor of a relay no longer listed is dropped (from the file too, once an account's cursors were read: written
   * on the ack lane, not the caller's thread).
   */
  void SetInboxes(std::vector<RelayInbox> inboxes, std::string profile_data_dir);
  /** A sync (scheduled, or `force`d by a wake) for `account_id`. */
  void Sync(bool force, const std::string& account_id);
  /** Ours, preferred first. */
  std::vector<RelayInbox> Inboxes() const;

private:
  struct Inbox {
    RelayInbox relay;
    bool preferred = false;
    std::atomic<bool> pending{false};
    std::atomic<bool> again{false};
    /** A permanent refusal's back-off (T14): not polled before refused_until_ms; 0: none. Guarded by mutex_. */
    int64_t refused_until_ms = 0;
    int64_t refused_backoff_ms = 0;
  };

  void Poll(const std::shared_ptr<Inbox>& inbox, const std::string& account_id);
  int64_t NowMs() const;
  /** `inbox` waits out a permanent refusal's back-off at `now_ms`. */
  bool BackingOff(const Inbox& inbox, int64_t now_ms) const;
  /** A poll of `inbox` was refused for good (true) or answered (false): its back-off grows or ends. */
  void NoteRefusal(Inbox& inbox, bool permanent);
  /** One poll of `inbox`: false when it failed. */
  bool PollOnce(const std::shared_ptr<Inbox>& inbox, const std::string& account_id);
  /** The cursor kept for `inbox`; none when it was kept for another `mailbox` than this one (dropped then). */
  std::string CursorOf(const Inbox& inbox, const std::string& account_id, const std::string& mailbox);
  void KeepCursor(const Inbox& inbox, const std::string& account_id, const std::string& cursor);
  /**
   * An older app's file's re-read at `inbox`, after a poll answered `messages` and `next_cursor`: phase 1 takes them all
   * and, once the inbox is read through (an empty page with no cursor), moves to phase 2 (the cursor dropped: true, the
   * page's cursor is not kept); phase 2 takes those stored since its floor and, read through, ends. None under way:
   * nothing changes (false).
   */
  bool ApplyReread(const Inbox& inbox, std::vector<RelayEnvelope>& messages, const std::string& next_cursor);
  /** Phase 2's floor for an older app's file last written at `written_ms`; none when that time cannot be trusted. */
  static std::optional<int64_t> RereadFloor(std::optional<int64_t> written_ms);
  void LoadCursorsLocked(const std::string& account_id);
  /** Drops the cursors of relays not listed and writes the file (cursors read for an account). */
  void SaveCursorsLocked();
  /** SaveCursorsLocked on the ack lane (called without the lock; the task takes it). */
  void SaveCursorsSoon();
  /** The inbox kept for `relay` at that place (same origin, client and role); none: it changed. */
  std::shared_ptr<Inbox> KeptLocked(const RelayInbox& relay, bool preferred) const;

  Ports ports_;
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Inbox>> inboxes_;
  std::string profile_data_dir_;
  std::string cursors_account_;
  /** The cursors read, by relay (RelayOriginKey). */
  std::map<std::string, std::string> cursors_;
  /** The mailbox each relay's cursor counts in (the registration id), by relay (RelayOriginKey). */
  std::map<std::string, std::string> mailboxes_;
  /** An older app's file's re-reads under way, by relay (RelayOriginKey). */
  std::map<std::string, RelayInboxReread> rereads_;
  /** The message ids phase 1 of a re-read took, by relay (RelayOriginKey); lost on a restart (duplicates only). */
  std::map<std::string, std::unordered_set<std::string>> phase1_taken_;
  uint64_t syncs_ = 0;
};

} // namespace pbr
