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
      mailboxes_.clear();
      rereads_.clear();
      phase1_taken_.clear();
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
  const std::string mailbox = ports_.mailbox ? ports_.mailbox(inbox->relay.origin, inbox->preferred) : std::string();
  auto poll = inbox->relay.client->PollInbox(account_id, CursorOf(*inbox, account_id, mailbox));
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
  const bool from_start = ApplyReread(*inbox, poll->messages, poll->next_cursor);
  // The cursor moves on before the next poll: a follow-up must not fetch the same batch while it waits for ingest.
  const std::string next_cursor = from_start ? std::string() : poll->next_cursor;
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
  mailboxes_.clear();
  rereads_.clear();
  phase1_taken_.clear();
  if (profile_data_dir_.empty()) {
    return;
  }
  const RelayInboxCursors kept = LoadRelayInboxCursors(profile_data_dir_, account_id);
  cursors_ = kept.others;
  mailboxes_ = kept.registrations.value_or(std::map<std::string, std::string>{});
  rereads_ = kept.rereads; // a re-read under way when the app stopped goes on where it was
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
  if (!kept.registrations && !cursors_.empty()) {
    // Kept by an app that did not note their mailbox: one may be gone (a revoke or a lapse, then a fresh registration:
    // a new mailbox numbered from the start, which the old cursor skips until its numbers pass it). Each relay with a
    // cursor is read again once, in two phases, and never gets less than the older app would have given it:
    // - phase 1 is exactly what the older app does: on from its cursor, unfiltered, until a page comes back empty. That
    //   takes in a backlog the older app left behind its cursor (it kept the cursor before the batch was taken in) and
    //   everything new, whatever the clocks say;
    // - phase 2 is the healing: the inbox from its start, until a page comes back empty, skipping the records the relay
    //   stored before the file's last write (less kRereadFloorSlackMs). Up to the old cursor the device took those in
    //   already, or the person cleared or deleted them before removed ids were kept (they would come back); what lay
    //   beyond it phase 1 took. A deaf device's missed messages came after that write, as its cursor stood still. A
    //   record with no store time is taken in. With a file time that cannot be trusted (unreadable, in the future,
    //   extreme) phase 2 takes everything: old chats may come back once, nothing is lost. Clock skew can only narrow
    //   what phase 2 heals, never what phase 1 delivers.
    // What was taken already is dropped as seen (message ids, removed ids, the control lane's seen ids).
    const std::optional<int64_t> floor =
        RereadFloor(ports_.cursors_written_ms ? ports_.cursors_written_ms(profile_data_dir_)
                                              : RelayInboxCursorFileWrittenMs(profile_data_dir_));
    log().warning << "relay inbox cursors kept without their mailbox: each inbox is read on, then once from its start"
                  << (floor ? "" : " (the file's time not trusted: everything in it)");
    DiagLine("relay.inbox_restart").Add("reason", "file").Emit();
    for (const auto& [relay, cursor] : cursors_) {
      rereads_[relay] = RelayInboxReread{.backfill = false, .floor_ms = floor};
    }
    SaveCursorsLocked();
  }
}

std::optional<int64_t> RelayInboxCoordinator::RereadFloor(const std::optional<int64_t> written_ms) {
  const int64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  if (!written_ms || *written_ms > now_ms || *written_ms <= kRereadFloorSlackMs) {
    return std::nullopt; // unreadable, in the future (a clock set back since, a copied profile), or extreme
  }
  return *written_ms - kRereadFloorSlackMs;
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
  const auto not_ours = [this](const auto& entry) {
    return std::none_of(inboxes_.begin(), inboxes_.end(), [&entry](const std::shared_ptr<Inbox>& known) {
      return RelayOriginKey(known->relay.origin) == entry.first;
    });
  };
  std::erase_if(cursors_, not_ours);
  std::erase_if(mailboxes_, not_ours);
  std::erase_if(rereads_, not_ours);
  std::erase_if(phase1_taken_, not_ours);
  if (profile_data_dir_.empty() || cursors_account_.empty()) {
    return;
  }
  RelayInboxCursors file;
  file.registrations = mailboxes_;
  file.rereads = rereads_;
  // Every relay's cursor by its origin, the preferred one's too (profile-model P3: no slot names a "preferred" relay).
  file.others = cursors_;
  SaveRelayInboxCursors(profile_data_dir_, cursors_account_, file);
}

std::string RelayInboxCoordinator::CursorOf(const Inbox& inbox, const std::string& account_id,
                                            const std::string& mailbox) {
  std::lock_guard lock(mutex_);
  LoadCursorsLocked(account_id);
  const std::string relay = RelayOriginKey(inbox.relay.origin);
  if (!mailbox.empty()) {
    std::string& kept_for = mailboxes_[relay];
    const auto reread = rereads_.find(relay);
    if (kept_for.empty() && reread != rereads_.end() && !reread->second.backfill) {
      kept_for = mailbox; // an older app's cursor in phase 1: kept, as that app keeps it; its mailbox noted from now on
      SaveCursorsLocked();
    } else if (kept_for != mailbox) {
      // A fresh registration there (a new mailbox, numbered from the start: a cursor from the old one skips it until
      // its numbers pass it), or a cursor kept while the registration there was not known. Read it from its start.
      if (!kept_for.empty()) {
        rereads_.erase(relay); // a new mailbox, read whole: nothing older in it to heal
        phase1_taken_.erase(relay);
      }
      if (cursors_.erase(relay) > 0) {
        log().warning << "relay inbox " << relay << ": its cursor was kept for "
                      << (kept_for.empty() ? "a registration not known then" : "another registration")
                      << "; reading the inbox from its start";
        DiagLine restart("relay.inbox_restart");
        if (kept_for.empty()) {
          restart.Add("reason", "unknown");
        } else {
          restart.Add("reason", "changed");
        }
        restart.Add("preferred", inbox.preferred).Emit();
      }
      kept_for = mailbox;
      SaveCursorsLocked();
    }
  }
  const auto it = cursors_.find(relay);
  return it == cursors_.end() ? std::string() : it->second;
}

bool RelayInboxCoordinator::ApplyReread(const Inbox& inbox, std::vector<RelayEnvelope>& messages,
                                        const std::string& next_cursor) {
  std::lock_guard lock(mutex_);
  const std::string relay = RelayOriginKey(inbox.relay.origin);
  const auto reread = rereads_.find(relay);
  if (reread == rereads_.end()) {
    return false;
  }
  // The relay has nothing further only when a page is empty and names no cursor: a page whose records were all
  // unreadable (skipped as it was read) comes back empty with a cursor, and the inbox goes on past it.
  const bool read_through = messages.empty() && next_cursor.empty();
  if (!reread->second.backfill) {
    if (!read_through) {
      // Phase 1: as the older app, everything. What it takes is noted, so phase 2 does not hand it in again (a late
      // duplicate re-applies some group events); at most kRereadTakenMax per relay, in memory only — beyond it, or
      // after a restart, phase 2 hands it in again and the receive pipeline drops it as seen.
      std::unordered_set<std::string>& taken = phase1_taken_[relay];
      for (const RelayEnvelope& envelope : messages) {
        if (!envelope.message_id.empty() && taken.size() < kRereadTakenMax) {
          taken.insert(envelope.message_id);
        }
      }
      return false;
    }
    reread->second.backfill = true; // drained: phase 2 reads the inbox from its start
    cursors_.erase(relay);
    log().info << "relay inbox " << relay << ": read on to its end; now once from its start";
    SaveCursorsLocked();
    return true;
  }
  if (read_through) {
    rereads_.erase(reread); // read through: from now on as any inbox
    phase1_taken_.erase(relay);
    log().info << "relay inbox " << relay << ": the re-read of an older app's file is done";
    SaveCursorsLocked();
    return false;
  }
  const std::optional<int64_t> floor_ms = reread->second.floor_ms;
  const auto taken = phase1_taken_.find(relay);
  const size_t before = messages.size();
  // The cursor still moves past what is skipped (the page's next_cursor).
  std::erase_if(messages, [&](const RelayEnvelope& envelope) {
    if (floor_ms && envelope.relay_created_at_ms && *envelope.relay_created_at_ms > 0 &&
        *envelope.relay_created_at_ms < *floor_ms) {
      return true;
    }
    return taken != phase1_taken_.end() && taken->second.contains(envelope.message_id);
  });
  if (messages.size() < before) {
    log().info << "relay inbox " << relay << ": re-read from its start skipped " << (before - messages.size())
               << " record(s): stored before an older app last wrote its file, or taken in already";
  }
  return false;
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
