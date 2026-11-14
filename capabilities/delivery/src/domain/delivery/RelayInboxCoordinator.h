#pragma once

#include "lib/delivery/RelayClients.h"
#include "lib/net/RelayInboxCursor.h"
#include "common/Module.h"
#include "common/chat/RelayEnvelope.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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
 * RelayInboxCursors) and one poll at a time; a batch is handed to `take` on the ingest lane, then acked on the relay
 * it came from. A message that arrives through two relays is taken twice: the receive pipeline drops the second by
 * its id. Thread-safe; the polls' HTTP runs on the `poll` lane.
 */
class RelayInboxCoordinator : public Module {
public:
  static constexpr int kOtherRelaysEverySyncs = 4;

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
  };

  explicit RelayInboxCoordinator(Ports ports);

  /** Ours, preferred first; `profile_data_dir` keeps the cursors (empty: in memory only). A poll under way ends. */
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
  };

  void Poll(const std::shared_ptr<Inbox>& inbox, const std::string& account_id);
  /** One poll of `inbox`: false when it failed. */
  bool PollOnce(const std::shared_ptr<Inbox>& inbox, const std::string& account_id);
  std::string CursorOf(const Inbox& inbox, const std::string& account_id);
  void KeepCursor(const Inbox& inbox, const std::string& account_id, const std::string& cursor);
  void LoadCursorsLocked(const std::string& account_id);

  Ports ports_;
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Inbox>> inboxes_;
  std::string profile_data_dir_;
  std::string cursors_account_;
  RelayInboxCursors cursors_;
  uint64_t syncs_ = 0;
};

} // namespace pbr
