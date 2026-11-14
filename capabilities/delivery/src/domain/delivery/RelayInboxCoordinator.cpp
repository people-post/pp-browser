#include "domain/delivery/RelayInboxCoordinator.h"

#include "common/Diag.h"
#include "common/chat/MessagingLimits.h"
#include "lib/delivery/RelayOrigins.h"

#include <algorithm>
#include <utility>

namespace pbr {

RelayInboxCoordinator::RelayInboxCoordinator(Ports ports) : ports_(std::move(ports)) {
  redirectLogger("RelayInboxCoordinator");
}

void RelayInboxCoordinator::SetInboxes(std::vector<RelayInbox> inboxes, std::string profile_data_dir) {
  std::vector<std::shared_ptr<Inbox>> next;
  for (RelayInbox& relay : inboxes) {
    if (!relay.client) {
      continue;
    }
    auto inbox = std::make_shared<Inbox>();
    inbox->preferred = next.empty();
    inbox->relay = std::move(relay);
    next.push_back(std::move(inbox));
  }
  std::lock_guard lock(mutex_);
  for (const auto& old : inboxes_) {
    old->again = false; // a poll under way polls once more at most, into its own (old) inbox
  }
  inboxes_ = std::move(next);
  profile_data_dir_ = std::move(profile_data_dir);
  cursors_account_.clear(); // read again for the next sync
  cursors_ = {};
}

std::vector<RelayInbox> RelayInboxCoordinator::Inboxes() const {
  std::lock_guard lock(mutex_);
  std::vector<RelayInbox> out;
  for (const auto& inbox : inboxes_) {
    out.push_back(inbox->relay);
  }
  return out;
}

void RelayInboxCoordinator::Sync(const bool force, const std::string& account_id) {
  if (account_id.empty()) {
    return;
  }
  std::vector<std::shared_ptr<Inbox>> due;
  {
    std::lock_guard lock(mutex_);
    const bool others = force || ++syncs_ % kOtherRelaysEverySyncs == 0;
    for (const auto& inbox : inboxes_) {
      if (inbox->preferred || others) {
        due.push_back(inbox);
      }
    }
  }
  for (const auto& inbox : due) {
    if (!inbox->preferred && ports_.ready && !ports_.ready(inbox->relay.origin)) {
      continue; // not registered there yet: it would refuse the poll
    }
    // Always mark a poll asked for: one in flight polls once more after it (a call's key may land meanwhile).
    inbox->again.store(true);
    if (ports_.poll) {
      ports_.poll([this, inbox, account_id]() { Poll(inbox, account_id); });
    }
  }
}

void RelayInboxCoordinator::Poll(const std::shared_ptr<Inbox>& inbox, const std::string& account_id) {
  bool expected = false;
  if (!inbox->pending.compare_exchange_strong(expected, true)) {
    return; // the one running sees `again`
  }
  struct PendingGuard {
    std::atomic<bool>& pending;
    ~PendingGuard() { pending = false; }
  } guard{inbox->pending};
  while (inbox->again.exchange(false)) {
    if (!PollOnce(inbox, account_id) && !inbox->preferred) {
      return; // another relay that failed waits for its next turn
    }
  }
}

bool RelayInboxCoordinator::PollOnce(const std::shared_ptr<Inbox>& inbox, const std::string& account_id) {
  auto poll = inbox->relay.client->PollInbox(account_id, CursorOf(*inbox, account_id));
  if (!poll) {
    log().warning << "PollInbox failed relay=" << RelayOriginKey(inbox->relay.origin) << ": " << poll.error().message;
    DiagLine("relay.poll_failed").Add("preferred", inbox->preferred).Fail("error", poll.error()).Emit();
    if (ports_.polled) {
      ports_.polled(inbox->relay.origin, inbox->preferred, &poll.error());
    }
    return false;
  }
  if (ports_.polled) {
    ports_.polled(inbox->relay.origin, inbox->preferred, nullptr);
  }
  if (poll->messages.size() > kMaxPollBatchMessages) {
    log().warning << "PollInbox batch too large n=" << poll->messages.size() << " — skip advance";
    DiagLine("relay.poll_too_large").Add("n", poll->messages.size()).Emit();
    return true;
  }
  // The cursor moves on before the next poll: a follow-up must not fetch the same batch while it waits for ingest.
  const std::string next_cursor = poll->next_cursor;
  if (!next_cursor.empty()) {
    KeepCursor(*inbox, account_id, next_cursor);
  }
  if (!poll->messages.empty()) {
    log().info << "PollInbox ok relay=" << RelayOriginKey(inbox->relay.origin) << " n=" << poll->messages.size()
               << " cursor_advanced=" << (next_cursor.empty() ? 0 : 1);
  }
  Batch batch{.origin = inbox->relay.origin, .preferred = inbox->preferred, .messages = std::move(poll->messages)};
  if (!ports_.ingest) {
    return true;
  }
  ports_.ingest([this, inbox, account_id, next_cursor, batch = std::move(batch)]() {
    if (ports_.take) {
      ports_.take(batch, account_id);
    }
    if (next_cursor.empty() || !ports_.ack) {
      return;
    }
    // Acked on the relay it came from, off the ingest lane (another HTTP round-trip).
    ports_.ack([inbox, account_id, next_cursor]() {
      auto ack = inbox->relay.client->AckInbox(account_id, next_cursor);
      if (!ack) {
        logging::getLogger("RelayInboxCoordinator").warning
            << "Relay inbox ack failed relay=" << RelayOriginKey(inbox->relay.origin) << ": " << ack.error().message;
      }
    });
  });
  return true;
}

void RelayInboxCoordinator::LoadCursorsLocked(const std::string& account_id) {
  if (cursors_account_ == account_id) {
    return;
  }
  cursors_account_ = account_id;
  cursors_ = profile_data_dir_.empty() ? RelayInboxCursors{} : LoadRelayInboxCursors(profile_data_dir_, account_id);
}

std::string RelayInboxCoordinator::CursorOf(const Inbox& inbox, const std::string& account_id) {
  std::lock_guard lock(mutex_);
  LoadCursorsLocked(account_id);
  if (inbox.preferred) {
    return cursors_.preferred;
  }
  const auto it = cursors_.others.find(RelayOriginKey(inbox.relay.origin));
  return it == cursors_.others.end() ? std::string() : it->second;
}

void RelayInboxCoordinator::KeepCursor(const Inbox& inbox, const std::string& account_id, const std::string& cursor) {
  std::lock_guard lock(mutex_);
  const bool current = std::any_of(inboxes_.begin(), inboxes_.end(),
                                   [&inbox](const std::shared_ptr<Inbox>& known) { return known.get() == &inbox; });
  if (!current) {
    return; // the inboxes changed meanwhile: this one's cursor is no longer ours to keep
  }
  LoadCursorsLocked(account_id);
  if (inbox.preferred) {
    cursors_.preferred = cursor;
  } else {
    cursors_.others[RelayOriginKey(inbox.relay.origin)] = cursor;
  }
  // A relay no longer ours keeps no cursor.
  std::erase_if(cursors_.others, [this](const auto& entry) {
    return std::none_of(inboxes_.begin(), inboxes_.end(), [&entry](const std::shared_ptr<Inbox>& known) {
      return !known->preferred && RelayOriginKey(known->relay.origin) == entry.first;
    });
  });
  if (!profile_data_dir_.empty()) {
    SaveRelayInboxCursors(profile_data_dir_, account_id, cursors_);
  }
}

} // namespace pbr
