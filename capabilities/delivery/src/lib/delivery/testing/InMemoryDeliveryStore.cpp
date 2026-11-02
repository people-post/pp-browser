#include "lib/delivery/testing/InMemoryDeliveryStore.h"

#include "common/Utilities.h"
#include "common/thread/ThreadChannel.h"

#include <algorithm>

namespace pbr {

namespace {

ThreadChannel ThreadChannelOf(const CryptoChannel channel) {
  return channel == CryptoChannel::E2ePublic ? ThreadChannel::E2ePublic : ThreadChannel::E2e;
}

DirectChatTarget TargetOf(const PeerSessionKey& key) {
  DirectChatTarget target;
  target.peer_identity_kind = key.peer_identity_kind;
  target.peer_identity_value = key.peer_identity_value;
  target.channel = ThreadChannelOf(key.channel);
  return target;
}

auto DeviceKeyOf(const PeerSessionKey& key) {
  return std::make_tuple(key.peer_identity_kind, key.peer_identity_value, key.channel, key.peer_device);
}

bool HasKeyMaterial(const PeerSessionRecord& record) {
  return record.master_psk_b64.has_value() || !record.retired_psks.empty();
}

} // namespace

InMemoryDeliveryStore::ChatTargetRow* InMemoryDeliveryStore::TargetLocked(const DirectChatTarget& target) {
  for (ChatTargetRow& row : targets_) {
    if (row.target == target) {
      return &row;
    }
  }
  return nullptr;
}

InMemoryDeliveryStore::ChatTargetRow* InMemoryDeliveryStore::TargetForThreadLocked(const std::string& thread_id) {
  for (ChatTargetRow& row : targets_) {
    if (row.thread_id == thread_id) {
      return &row;
    }
  }
  return nullptr;
}

const InMemoryDeliveryStore::ChatTargetRow* InMemoryDeliveryStore::TargetForThreadLocked(
    const std::string& thread_id) const {
  return const_cast<InMemoryDeliveryStore*>(this)->TargetForThreadLocked(thread_id);
}

InMemoryDeliveryStore::Numbering* InMemoryDeliveryStore::NumberingLocked(const std::string& thread_id) {
  auto thread = threads_.find(thread_id);
  if (thread == threads_.end()) {
    return nullptr;
  }
  if (thread->second.kind == ThreadKind::Group && thread->second.group_id) {
    return &groups_[*thread->second.group_id];
  }
  ChatTargetRow* row = TargetForThreadLocked(thread_id);
  return row ? &row->numbering : nullptr;
}

const InMemoryDeliveryStore::Numbering* InMemoryDeliveryStore::NumberingLocked(const std::string& thread_id) const {
  return const_cast<InMemoryDeliveryStore*>(this)->NumberingLocked(thread_id);
}

std::optional<Thread> InMemoryDeliveryStore::FindDirectThreadLocked(const DirectChatTarget& target) const {
  for (const ChatTargetRow& row : targets_) {
    if (row.target == target && !row.thread_id.empty()) {
      auto thread = threads_.find(row.thread_id);
      if (thread != threads_.end()) {
        return thread->second;
      }
    }
  }
  return std::nullopt;
}

Thread InMemoryDeliveryStore::UpsertThreadLocked(const Thread& thread) {
  Thread stored = thread;
  if (auto existing = threads_.find(thread.id); existing != threads_.end()) {
    stored.listed = existing->second.listed || thread.listed;  // as SQLite: listed stays once set
  }
  threads_[stored.id] = stored;
  return stored;
}

Roe<ThreadMessage> InMemoryDeliveryStore::AppendMessageLocked(const ThreadMessage& message) {
  std::vector<ThreadMessage>& rows = messages_[message.thread_id];
  for (const ThreadMessage& row : rows) {
    if (row.id == message.id) {
      return Error("Message id already in thread: " + message.id);
    }
  }
  ThreadMessage stored = message;
  if (stored.display_order <= 0) {
    stored.display_order = rows.empty() ? 1 : rows.back().display_order + 1;
  }
  auto at = std::upper_bound(rows.begin(), rows.end(), stored.display_order,
                             [](int64_t order, const ThreadMessage& row) { return order < row.display_order; });
  rows.insert(at, stored);
  return stored;
}

Roe<std::vector<Thread>> InMemoryDeliveryStore::ListThreads() const {
  std::lock_guard lock(mutex_);
  std::vector<Thread> out;
  for (const auto& [id, thread] : threads_) {
    out.push_back(thread);
  }
  std::sort(out.begin(), out.end(), [](const Thread& a, const Thread& b) { return a.updated_at > b.updated_at; });
  return out;
}

Roe<std::optional<Thread>> InMemoryDeliveryStore::GetThread(const std::string& thread_id) const {
  std::lock_guard lock(mutex_);
  auto thread = threads_.find(thread_id);
  if (thread == threads_.end()) {
    return std::optional<Thread>();
  }
  return std::optional<Thread>(thread->second);
}

Roe<Thread> InMemoryDeliveryStore::UpsertThread(const Thread& thread) {
  std::lock_guard lock(mutex_);
  return UpsertThreadLocked(thread);
}

Roe<bool> InMemoryDeliveryStore::DeleteThread(const std::string& thread_id) {
  std::lock_guard lock(mutex_);
  const bool existed = threads_.erase(thread_id) > 0;
  messages_.erase(thread_id);
  memory_.erase(thread_id);
  return existed;
}

Roe<std::optional<Thread>> InMemoryDeliveryStore::FindDirectThread(const DirectChatTarget& target) const {
  std::lock_guard lock(mutex_);
  return FindDirectThreadLocked(target);
}

Roe<Thread> InMemoryDeliveryStore::FindOrCreateDirectThread(const DirectChatTarget& target,
                                                          const std::string& participant_contact_id,
                                                          const std::string& title) {
  std::lock_guard lock(mutex_);
  if (std::optional<Thread> existing = FindDirectThreadLocked(target)) {
    Thread thread = *existing;
    if (!participant_contact_id.empty() && thread.participant_contact_ids.empty()) {
      thread.participant_contact_ids = {participant_contact_id};
      if (!title.empty()) {
        thread.title = title;
      }
      thread.updated_at = util::NowUnixMs();
      thread = UpsertThreadLocked(thread);
    }
    if (!participant_contact_id.empty()) {
      if (ChatTargetRow* row = TargetForThreadLocked(thread.id)) {
        row->participant_contact_id = participant_contact_id;
      }
    }
    return thread;
  }
  Thread thread;
  thread.id = util::GenerateUuid();
  thread.kind = ThreadKind::Direct;
  thread.channel = target.channel;
  thread.peer_identity_kind = target.peer_identity_kind;
  thread.peer_identity_value = target.peer_identity_value;
  if (!participant_contact_id.empty()) {
    thread.participant_contact_ids = {participant_contact_id};
  }
  thread.title = title;
  thread.encrypted = ThreadChannelIsE2e(target.channel);
  thread.updated_at = util::NowUnixMs();
  thread.listed = false;  // listed once something readable is in it, or the user opens it
  Thread saved = UpsertThreadLocked(thread);
  // The session with this peer may have come first (its row, its epoch): the thread joins that row.
  ChatTargetRow* row = TargetLocked(target);
  if (!row) {
    targets_.push_back(ChatTargetRow{});
    row = &targets_.back();
    row->target = target;
  }
  row->participant_contact_id = participant_contact_id;
  row->thread_id = saved.id;
  return saved;
}

Roe<std::optional<Thread>> InMemoryDeliveryStore::FindGroupThread(const std::string& group_id) const {
  std::lock_guard lock(mutex_);
  for (const auto& [id, thread] : threads_) {
    if (thread.kind == ThreadKind::Group && thread.group_id == group_id) {
      return std::optional<Thread>(thread);
    }
  }
  return std::optional<Thread>();
}

Roe<Thread> InMemoryDeliveryStore::FindOrCreateGroupThread(const std::string& group_id, const std::string& title,
                                                         const std::vector<std::string>& participant_contact_ids) {
  if (auto existing = FindGroupThread(group_id); existing && *existing) {
    return **existing;
  }
  std::lock_guard lock(mutex_);
  Thread thread;
  thread.id = util::GenerateUuid();
  thread.kind = ThreadKind::Group;
  thread.channel = ThreadChannel::None;
  thread.group_id = group_id;
  thread.participant_contact_ids = participant_contact_ids;
  thread.title = title;
  thread.encrypted = true;
  thread.updated_at = util::NowUnixMs();
  groups_.try_emplace(group_id);
  return UpsertThreadLocked(thread);
}

Roe<std::vector<ThreadMessage>> InMemoryDeliveryStore::GetMessages(const std::string& thread_id) const {
  std::lock_guard lock(mutex_);
  auto rows = messages_.find(thread_id);
  return rows == messages_.end() ? std::vector<ThreadMessage>{} : rows->second;
}

Roe<std::vector<ThreadMessage>> InMemoryDeliveryStore::GetMessagesPage(const std::string& thread_id,
                                                                     std::optional<int64_t> before_display_order,
                                                                     size_t limit) const {
  std::lock_guard lock(mutex_);
  std::vector<ThreadMessage> page;
  auto rows = messages_.find(thread_id);
  if (rows == messages_.end()) {
    return page;
  }
  // The newest `limit` (below `before_display_order`), in ascending order — as SQLite's DESC + reverse.
  for (auto it = rows->second.rbegin(); it != rows->second.rend() && page.size() < limit; ++it) {
    if (!before_display_order || it->display_order < *before_display_order) {
      page.push_back(*it);
    }
  }
  std::reverse(page.begin(), page.end());
  return page;
}

Roe<std::vector<ThreadMessage>> InMemoryDeliveryStore::GetMessagesForContext(const std::string& thread_id,
                                                                           const ContextBudget& /*budget*/) const {
  return GetMessages(thread_id);
}

Roe<int64_t> InMemoryDeliveryStore::CountContextEligibleMessagesAfter(const std::string& thread_id,
                                                                    int64_t after_display_order) const {
  auto after = GetContextEligibleMessagesAfter(thread_id, after_display_order);
  if (!after) {
    return after.error();
  }
  return static_cast<int64_t>(after->size());
}

Roe<int64_t> InMemoryDeliveryStore::CountAnnotationsForTarget(const std::string& thread_id,
                                                            const std::string& target_message_id) const {
  auto annotations = GetAnnotationsForTarget(thread_id, target_message_id);
  if (!annotations) {
    return annotations.error();
  }
  return static_cast<int64_t>(annotations->size());
}

Roe<std::vector<ThreadMessage>> InMemoryDeliveryStore::GetContextEligibleMessagesAfter(
    const std::string& thread_id, int64_t after_display_order) const {
  std::lock_guard lock(mutex_);
  std::vector<ThreadMessage> out;
  if (auto rows = messages_.find(thread_id); rows != messages_.end()) {
    for (const ThreadMessage& row : rows->second) {
      if (row.display_order > after_display_order) {
        out.push_back(row);
      }
    }
  }
  return out;
}

Roe<ThreadMessage> InMemoryDeliveryStore::AppendMessage(const ThreadMessage& message) {
  std::lock_guard lock(mutex_);
  return AppendMessageLocked(message);
}

Roe<bool> InMemoryDeliveryStore::UpdateMessage(const ThreadMessage& message) {
  std::lock_guard lock(mutex_);
  auto rows = messages_.find(message.thread_id);
  if (rows == messages_.end()) {
    return false;
  }
  for (ThreadMessage& row : rows->second) {
    if (row.id == message.id) {
      const int64_t order = row.display_order;  // keeps its place in the thread
      row = message;
      row.display_order = order;
      return true;
    }
  }
  return false;
}

Roe<bool> InMemoryDeliveryStore::HasMessageId(const std::string& thread_id, const std::string& message_id) const {
  std::lock_guard lock(mutex_);
  if (auto rows = messages_.find(thread_id); rows != messages_.end()) {
    for (const ThreadMessage& row : rows->second) {
      if (row.id == message_id) {
        return true;
      }
    }
  }
  return false;
}

Roe<void> InMemoryDeliveryStore::ClearMessages(const std::string& thread_id, const ClearMessagesOptions& options) {
  std::lock_guard lock(mutex_);
  messages_.erase(thread_id);
  if (options.forget_memory) {
    memory_.erase(thread_id);
  }
  return {};
}

Roe<std::vector<ThreadMessage>> InMemoryDeliveryStore::ExportMessagesUpTo(
    const std::string& thread_id, const std::optional<std::string>& max_message_id) const {
  auto messages = GetMessages(thread_id);
  if (!messages) {
    return messages.error();
  }
  std::vector<ThreadMessage> exported;
  for (const ThreadMessage& message : *messages) {
    if (message.content_type != ChatContentType::Deleted) {
      exported.push_back(message);
    }
    if (max_message_id && message.id == *max_message_id) {
      break;
    }
  }
  return exported;
}

Roe<std::optional<ConversationSummary>> InMemoryDeliveryStore::GetThreadMemory(const std::string& thread_id) const {
  std::lock_guard lock(mutex_);
  auto summary = memory_.find(thread_id);
  if (summary == memory_.end()) {
    return std::optional<ConversationSummary>();
  }
  return std::optional<ConversationSummary>(summary->second);
}

Roe<void> InMemoryDeliveryStore::SetThreadMemory(const std::string& thread_id, const ConversationSummary& summary) {
  std::lock_guard lock(mutex_);
  memory_[thread_id] = summary;
  return {};
}

Roe<void> InMemoryDeliveryStore::ClearThreadMemory(const std::string& thread_id) {
  std::lock_guard lock(mutex_);
  memory_.erase(thread_id);
  return {};
}

Roe<uint64_t> InMemoryDeliveryStore::AllocateSenderSeq(const std::string& thread_id) {
  std::lock_guard lock(mutex_);
  if (threads_.find(thread_id) == threads_.end()) {
    return Error("Thread not found");
  }
  Numbering* numbering = NumberingLocked(thread_id);
  if (!numbering) {
    return Error("No chat target for thread");
  }
  return numbering->next_outgoing_seq++;
}

Roe<uint32_t> InMemoryDeliveryStore::GetChatTargetSessionEpoch(const std::string& thread_id) const {
  std::lock_guard lock(mutex_);
  if (threads_.find(thread_id) == threads_.end()) {
    return Error("Thread not found");
  }
  const Numbering* numbering = NumberingLocked(thread_id);
  if (!numbering) {
    return Error("No chat target for thread");
  }
  return numbering->session_epoch;
}

Roe<std::vector<ThreadMessage>> InMemoryDeliveryStore::GetMessagesBySeqRange(const std::string& thread_id,
                                                                           const SeqRangeQuery& query) const {
  auto messages = GetMessages(thread_id);
  if (!messages) {
    return messages.error();
  }
  std::vector<ThreadMessage> out;
  for (const ThreadMessage& message : *messages) {
    if (!message.sender_seq || message.session_epoch.value_or(1) != query.session_epoch ||
        message.seq_owner_contact_id.value_or(std::string()) != query.seq_owner_contact_id ||
        message.sender_device.value_or(std::string()) != query.sender_device) {
      continue;
    }
    if ((query.min_sender_seq && *message.sender_seq < *query.min_sender_seq) ||
        (query.max_sender_seq && *message.sender_seq > *query.max_sender_seq)) {
      continue;
    }
    out.push_back(message);
  }
  std::sort(out.begin(), out.end(), [&](const ThreadMessage& a, const ThreadMessage& b) {
    return query.ascending ? *a.sender_seq < *b.sender_seq : *a.sender_seq > *b.sender_seq;
  });
  if (out.size() > query.limit) {
    out.resize(query.limit);
  }
  return out;
}

Roe<PeerSyncState> InMemoryDeliveryStore::GetPeerSyncState(const std::string& thread_id, uint32_t session_epoch,
                                                         const std::string& sender_device) const {
  std::lock_guard lock(mutex_);
  auto state = sync_.find({thread_id, session_epoch, sender_device});
  return state == sync_.end() ? PeerSyncState{} : state->second;
}

Roe<void> InMemoryDeliveryStore::SetPeerSyncState(const std::string& thread_id, uint32_t session_epoch,
                                                const PeerSyncState& state, const std::string& sender_device) {
  std::lock_guard lock(mutex_);
  sync_[{thread_id, session_epoch, sender_device}] = state;
  return {};
}

Roe<void> InMemoryDeliveryStore::CancelOldEpochPending(const std::string& /*thread_id*/,
                                                     uint32_t /*old_session_epoch*/) {
  return {};  // no outbox here
}

Roe<void> InMemoryDeliveryStore::AdoptChatTargetEpoch(const std::string& thread_id, uint32_t new_session_epoch) {
  std::lock_guard lock(mutex_);
  Numbering* numbering = NumberingLocked(thread_id);
  if (!numbering) {
    return Error("No chat target for thread");
  }
  numbering->session_epoch = new_session_epoch;
  return {};
}

Roe<ThreadMessage> InMemoryDeliveryStore::AppendMessageWithPassiveEpochAdopt(const ThreadMessage& message,
                                                                           uint32_t /*old_session_epoch*/,
                                                                           uint32_t new_session_epoch,
                                                                           const PeerSyncState& new_sync_state) {
  std::lock_guard lock(mutex_);
  if (Numbering* numbering = NumberingLocked(message.thread_id)) {
    numbering->session_epoch = new_session_epoch;
  }
  sync_[{message.thread_id, new_session_epoch, message.sender_device.value_or(std::string())}] = new_sync_state;
  return AppendMessageLocked(message);
}

Roe<uint32_t> InMemoryDeliveryStore::BumpLocalChatTargetEpoch(const std::string& thread_id) {
  std::lock_guard lock(mutex_);
  Numbering* numbering = NumberingLocked(thread_id);
  if (!numbering) {
    return Error("No chat target for thread");
  }
  return ++numbering->session_epoch;
}

Roe<void> InMemoryDeliveryStore::ReconcileOutbox() {
  return {};
}

Roe<std::vector<std::pair<std::string, std::string>>> InMemoryDeliveryStore::ListPendingOutbox() const {
  return std::vector<std::pair<std::string, std::string>>{};
}

Roe<std::optional<PeerSessionRecord>> InMemoryDeliveryStore::Load(const PeerSessionKey& key) const {
  std::lock_guard lock(mutex_);
  if (!key.peer_device.empty()) {
    auto device = devices_.find(DeviceKeyOf(key));
    if (device == devices_.end()) {
      return std::optional<PeerSessionRecord>();
    }
    return std::optional<PeerSessionRecord>(device->second.session);
  }
  const ChatTargetRow* row = const_cast<InMemoryDeliveryStore*>(this)->TargetLocked(TargetOf(key));
  if (!row) {
    return std::optional<PeerSessionRecord>();
  }
  // A thread's row without a session yet still answers, with no key (as SQLite's chat_targets row does).
  PeerSessionRecord record = row->session.value_or(PeerSessionRecord{});
  record.key = key;
  record.session_epoch = row->numbering.session_epoch;
  return std::optional<PeerSessionRecord>(record);
}

Roe<std::vector<PeerSessionRecord>> InMemoryDeliveryStore::List() const {
  std::lock_guard lock(mutex_);
  std::vector<PeerSessionRecord> out;
  for (const ChatTargetRow& row : targets_) {
    if (row.session && HasKeyMaterial(*row.session)) {
      PeerSessionRecord record = *row.session;
      record.session_epoch = row.numbering.session_epoch;
      out.push_back(record);
    }
  }
  for (const auto& [key, device] : devices_) {
    if (HasKeyMaterial(device.session)) {
      out.push_back(device.session);
    }
  }
  return out;
}

Roe<void> InMemoryDeliveryStore::Save(const PeerSessionRecord& record) {
  std::lock_guard lock(mutex_);
  if (!record.key.peer_device.empty()) {
    DeviceSessionRow& device = devices_[DeviceKeyOf(record.key)];
    if (device.session.session_epoch != record.session_epoch) {
      device.next_outgoing_seq = 1;  // a new epoch numbers from 1
    }
    device.session = record;
    return {};
  }
  const DirectChatTarget target = TargetOf(record.key);
  ChatTargetRow* row = TargetLocked(target);
  if (!row) {
    targets_.push_back(ChatTargetRow{});
    row = &targets_.back();
    row->target = target;
  }
  row->session = record;
  row->numbering.session_epoch = record.session_epoch;
  return {};
}

Roe<uint64_t> InMemoryDeliveryStore::AllocateSenderSeq(const PeerSessionKey& key) {
  if (key.peer_device.empty()) {
    return Error("AllocateSenderSeq: a device-pair session only (the account's numbering is the thread's)");
  }
  std::lock_guard lock(mutex_);
  auto device = devices_.find(DeviceKeyOf(key));
  if (device == devices_.end()) {
    return Error("No device session to number in");
  }
  return device->second.next_outgoing_seq++;
}

Roe<bool> InMemoryControlLaneSeenStore::FirstSight(const std::string& sender, const std::string& control_id,
                                                   int64_t /*now_ms*/) {
  std::lock_guard lock(mutex_);
  return seen_.insert({sender, control_id}).second;
}

void InMemoryControlLaneSeenStore::Forget(const std::string& sender, const std::string& control_id) {
  std::lock_guard lock(mutex_);
  seen_.erase({sender, control_id});
}

} // namespace pbr
