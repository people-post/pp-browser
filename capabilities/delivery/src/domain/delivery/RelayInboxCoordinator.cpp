#include "domain/delivery/RelayInboxCoordinator.h"

#include "common/Diag.h"
#include "common/chat/MessagingLimits.h"
#include "lib/net/RelayAudience.h"
#include "lib/net/RelayOrigins.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace pbr {

RelayInboxCoordinator::RelayInboxCoordinator(Ports ports) : ports_(std::move(ports)) {
  redirectLogger("RelayInboxCoordinator");
}

std::shared_ptr<RelayInboxCoordinator::Inbox> RelayInboxCoordinator::KeptLocked(const RelayInbox& relay,
                                                                                const bool preferred) const {
  const auto kept = std::find_if(inboxes_.begin(), inboxes_.end(), [&](const std::shared_ptr<Inbox>& inbox) {
    return inbox->relay.client == relay.client && inbox->preferred == preferred &&
           RelayOriginKey(inbox->relay.origin) == RelayOriginKey(relay.origin);
  });
  return kept == inboxes_.end() ? nullptr : *kept;
}

void RelayInboxCoordinator::SetInboxes(std::vector<RelayInbox> inboxes, std::string profile_data_dir) {
  bool save = false;
  {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<Inbox>> next;
    for (RelayInbox& relay : inboxes) {
      if (!relay.client) {
        continue;
      }
      if (auto kept = KeptLocked(relay, next.empty())) {
        next.push_back(std::move(kept)); // unchanged: a poll under way keeps its cursor and its follow-up
        continue;
      }
      auto inbox = std::make_shared<Inbox>();
      inbox->preferred = next.empty();
      inbox->relay = std::move(relay);
      next.push_back(std::move(inbox));
    }
    for (const auto& old : inboxes_) {
      if (std::find(next.begin(), next.end(), old) == next.end()) {
        old->again = false; // a poll under way polls once more at most, into its own (old) inbox
      }
    }
    const bool changed = next != inboxes_;
    inboxes_ = std::move(next);
    if (!cursors_account_.empty() && profile_data_dir_ == profile_data_dir) {
      save = changed; // a relay no longer ours keeps no cursor; the one made preferred keeps its own
    } else {
      profile_data_dir_ = std::move(profile_data_dir);
      cursors_account_.clear(); // read again for the next sync
      cursors_.clear();
    }
  }
  if (save) {
    SaveCursorsSoon();
  }
}

int64_t RelayInboxCoordinator::NowMs() const {
  if (ports_.now_ms) {
    return ports_.now_ms();
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool RelayInboxCoordinator::BackingOff(const Inbox& inbox, const int64_t now_ms) const {
  std::lock_guard lock(mutex_);
  return inbox.refused_until_ms > 0 && now_ms < inbox.refused_until_ms;
}

void RelayInboxCoordinator::NoteRefusal(Inbox& inbox, const bool permanent) {
  std::lock_guard lock(mutex_);
  if (!permanent) {
    inbox.refused_until_ms = 0;
    inbox.refused_backoff_ms = 0;
    return;
  }
  inbox.refused_backoff_ms =
      inbox.refused_backoff_ms == 0 ? kRefusedFirstMs : std::min(inbox.refused_backoff_ms * 2, kRefusedMaxMs);
  inbox.refused_until_ms = NowMs() + inbox.refused_backoff_ms;
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
  const int64_t now = NowMs();
  for (const auto& inbox : due) {
    if (!inbox->preferred && ports_.ready && !ports_.ready(inbox->relay.origin)) {
      continue; // not registered there yet: it would refuse the poll
    }
    if (BackingOff(*inbox, now)) {
      continue; // it refused us for good a moment ago (T14): asking every 2 s changes nothing
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
    if (!PollOnce(inbox, account_id) && (!inbox->preferred || BackingOff(*inbox, NowMs()))) {
      return; // another relay that failed, or one refusing us for good, waits for its next turn
    }
  }
}

bool RelayInboxCoordinator::PollOnce(const std::shared_ptr<Inbox>& inbox, const std::string& account_id) {
  auto poll = inbox->relay.client->PollInbox(account_id, CursorOf(*inbox, account_id));
  NoteRefusal(*inbox, !poll && IsPermanentRelayRefusal(poll.error()));
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
  cursors_.clear();
  if (profile_data_dir_.empty()) {
    return;
  }
  const RelayInboxCursors kept = LoadRelayInboxCursors(profile_data_dir_, account_id);
  cursors_ = kept.others;
  if (!kept.preferred.empty()) {
    // A file from before My relays names no relay for it: the preferred one's.
    std::string relay = kept.preferred_relay;
    if (relay.empty() && !inboxes_.empty()) {
      relay = RelayOriginKey(inboxes_.front()->relay.origin);
    }
    if (!relay.empty()) {
      cursors_[relay] = kept.preferred;
    }
  }
}

void RelayInboxCoordinator::SaveCursorsSoon() {
  // Off the caller's thread (the hub's): a file write, under the lock a poll holds for its own.
  const auto save = [this]() {
    std::lock_guard lock(mutex_);
    SaveCursorsLocked();
  };
  if (ports_.ack) {
    ports_.ack(save);
    return;
  }
  save();
}

void RelayInboxCoordinator::SaveCursorsLocked() {
  std::erase_if(cursors_, [this](const auto& entry) {
    return std::none_of(inboxes_.begin(), inboxes_.end(), [&entry](const std::shared_ptr<Inbox>& known) {
      return RelayOriginKey(known->relay.origin) == entry.first;
    });
  });
  if (profile_data_dir_.empty() || cursors_account_.empty()) {
    return;
  }
  RelayInboxCursors file;
  for (const auto& [relay, cursor] : cursors_) {
    if (!inboxes_.empty() && relay == RelayOriginKey(inboxes_.front()->relay.origin)) {
      file.preferred = cursor;
      file.preferred_relay = relay;
    } else {
      file.others[relay] = cursor;
    }
  }
  SaveRelayInboxCursors(profile_data_dir_, cursors_account_, file);
}

std::string RelayInboxCoordinator::CursorOf(const Inbox& inbox, const std::string& account_id) {
  std::lock_guard lock(mutex_);
  LoadCursorsLocked(account_id);
  const auto it = cursors_.find(RelayOriginKey(inbox.relay.origin));
  return it == cursors_.end() ? std::string() : it->second;
}

void RelayInboxCoordinator::KeepCursor(const Inbox& inbox, const std::string& account_id, const std::string& cursor) {
  std::lock_guard lock(mutex_);
  const bool current = std::any_of(inboxes_.begin(), inboxes_.end(),
                                   [&inbox](const std::shared_ptr<Inbox>& known) { return known.get() == &inbox; });
  if (!current) {
    return; // the inboxes changed meanwhile: this one's cursor is no longer ours to keep
  }
  LoadCursorsLocked(account_id);
  cursors_[RelayOriginKey(inbox.relay.origin)] = cursor;
  SaveCursorsLocked();
}

} // namespace pbr
