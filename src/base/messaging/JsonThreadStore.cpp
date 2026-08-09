#include "base/messaging/JsonThreadStore.h"

#include "base/data/AtomicFileWrite.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/MessagingLimits.h"
#include "base/messaging/SyncStateCodec.h"
#include "common/Utilities.h"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

std::string ThreadsDir(const std::string& data_dir) {
  return (std::filesystem::path(data_dir) / "threads").string();
}

} // namespace

JsonThreadStore::JsonThreadStore(std::string data_dir) : data_dir_(std::move(data_dir)) {
  redirectLogger("JsonThreadStore");
}

std::string JsonThreadStore::IndexPath() const {
  return (std::filesystem::path(ThreadsDir(data_dir_)) / "index.json").string();
}

std::string JsonThreadStore::ThreadPath(const std::string& thread_id) const {
  return (std::filesystem::path(ThreadsDir(data_dir_)) / (thread_id + ".json")).string();
}

Roe<void> JsonThreadStore::EnsureLoaded() const {
  if (loaded_) {
    return {};
  }

  std::error_code ec;
  std::filesystem::create_directories(ThreadsDir(data_dir_), ec);

  threads_.clear();
  messages_.clear();

  std::ifstream index_in(IndexPath());
  if (index_in) {
    const nlohmann::json root = nlohmann::json::parse(index_in, nullptr, false);
    if (!root.is_discarded() && root.contains("threads") && root["threads"].is_array()) {
      for (const auto& item : root["threads"]) {
        threads_.push_back(ThreadFromJson(item));
      }
    }
  }

  for (const Thread& thread : threads_) {
    std::ifstream message_in(ThreadPath(thread.id));
    if (!message_in) {
      messages_[thread.id] = {};
      continue;
    }
    const nlohmann::json root = nlohmann::json::parse(message_in, nullptr, false);
    if (root.is_discarded() || !root.contains("messages") || !root["messages"].is_array()) {
      messages_[thread.id] = {};
      continue;
    }
    auto& list = messages_[thread.id];
    for (const auto& item : root["messages"]) {
      ThreadMessage message = ThreadMessageFromJson(item);
      list.push_back(message);
    }
  }

  loaded_ = true;
  return {};
}

Roe<void> JsonThreadStore::SaveIndex() const {
  nlohmann::json threads = nlohmann::json::array();
  for (const Thread& thread : threads_) {
    threads.push_back(ThreadToJson(thread));
  }
  const nlohmann::json root = {{"threads", std::move(threads)}};

  return AtomicFileWrite::Write(IndexPath(), root.dump(2));
}

Roe<void> JsonThreadStore::SaveMessages(const std::string& thread_id) const {
  const auto it = messages_.find(thread_id);
  nlohmann::json messages = nlohmann::json::array();
  if (it != messages_.end()) {
    for (const ThreadMessage& message : it->second) {
      messages.push_back(ThreadMessageToJson(message));
    }
  }
  const nlohmann::json root = {{"messages", std::move(messages)}};

  return AtomicFileWrite::Write(ThreadPath(thread_id), root.dump(2));
}

void JsonThreadStore::Flush() {
  std::lock_guard lock(mutex_);
  if (!dirty_) {
    return;
  }
  if (auto result = SaveIndex()) {
    dirty_ = false;
    for (const auto& [thread_id, _] : messages_) {
      (void)SaveMessages(thread_id);
    }
  }
}

Roe<std::vector<Thread>> JsonThreadStore::ListThreads() const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  return threads_;
}

Roe<std::optional<Thread>> JsonThreadStore::GetThread(const std::string& thread_id) const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  for (const Thread& thread : threads_) {
    if (thread.id == thread_id) {
      return Roe<std::optional<Thread>>(thread);
    }
  }
  return Roe<std::optional<Thread>>(std::optional<Thread>{});
}

Roe<Thread> JsonThreadStore::UpsertThread(const Thread& thread) {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }

  bool found = false;
  for (Thread& existing : threads_) {
    if (existing.id == thread.id) {
      existing = thread;
      found = true;
      break;
    }
  }
  if (!found) {
    threads_.push_back(thread);
    messages_[thread.id] = {};
  }

  dirty_ = true;
  if (auto save = SaveIndex()) {
    dirty_ = false;
    if (!found) {
      (void)SaveMessages(thread.id);
    }
    return thread;
  }
  return Error("Failed to save thread index");
}

Roe<std::vector<ThreadMessage>> JsonThreadStore::GetMessages(const std::string& thread_id) const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  const auto it = messages_.find(thread_id);
  if (it == messages_.end()) {
    return std::vector<ThreadMessage>{};
  }
  return it->second;
}

Roe<ThreadMessage> JsonThreadStore::AppendMessage(const ThreadMessage& message) {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }

  ThreadMessage stored = message;
  if (stored.display_order <= 0) {
    stored.display_order = NextDisplayOrder(message.thread_id);
  }
  messages_[message.thread_id].push_back(stored);
  dirty_ = true;
  if (auto save = SaveMessages(message.thread_id)) {
    return stored;
  }
  return Error("Failed to append message");
}

int64_t JsonThreadStore::NextDisplayOrder(const std::string& thread_id) const {
  const auto it = messages_.find(thread_id);
  if (it == messages_.end() || it->second.empty()) {
    return 1;
  }
  int64_t max_order = 0;
  for (const ThreadMessage& message : it->second) {
    max_order = std::max(max_order, message.display_order);
  }
  return max_order + 1;
}

Roe<std::vector<ThreadMessage>> JsonThreadStore::GetMessagesPage(const std::string& thread_id,
                                                                 std::optional<int64_t> before_display_order,
                                                                 size_t limit) const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  const auto it = messages_.find(thread_id);
  if (it == messages_.end()) {
    return std::vector<ThreadMessage>{};
  }
  std::vector<ThreadMessage> sorted = it->second;
  std::sort(sorted.begin(), sorted.end(),
            [](const ThreadMessage& a, const ThreadMessage& b) { return a.display_order < b.display_order; });
  std::vector<ThreadMessage> page;
  for (auto rit = sorted.rbegin(); rit != sorted.rend() && page.size() < limit; ++rit) {
    if (before_display_order.has_value() && rit->display_order >= *before_display_order) {
      continue;
    }
    page.push_back(*rit);
  }
  std::reverse(page.begin(), page.end());
  return page;
}

Roe<std::vector<ThreadMessage>> JsonThreadStore::GetMessagesForContext(const std::string& thread_id,
                                                                       const ContextBudget& budget) const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }

  int64_t compaction_cursor = 0;
  if (const auto memory_it = memory_.find(thread_id); memory_it != memory_.end()) {
    if (memory_it->second.compacted_through_display_order.has_value()) {
      compaction_cursor = *memory_it->second.compacted_through_display_order;
    }
  }

  const auto it = messages_.find(thread_id);
  if (it == messages_.end()) {
    return std::vector<ThreadMessage>{};
  }

  std::vector<ThreadMessage> eligible;
  for (const ThreadMessage& message : it->second) {
    if (message.content_type != ChatContentType::Text && message.content_type != ChatContentType::System) {
      continue;
    }
    if (message.display_order <= compaction_cursor) {
      continue;
    }
    eligible.push_back(message);
  }
  std::sort(eligible.begin(), eligible.end(),
            [](const ThreadMessage& a, const ThreadMessage& b) { return a.display_order > b.display_order; });

  const size_t min_keep = static_cast<size_t>(kCompactionMinTurnsKept * 2);
  std::vector<ThreadMessage> selected;
  int char_budget = budget.max_recent_chars;
  for (size_t i = 0; i < eligible.size(); ++i) {
    const ThreadMessage& message = eligible[i];
    const bool below_min = selected.size() < min_keep;
    if (!below_min && static_cast<int>(selected.size()) >= budget.max_turn_pairs * 2) {
      break;
    }
    const int line_size = static_cast<int>(message.text.size() + message.sender_contact_id.size() + 2);
    if (!below_min && char_budget - line_size < 0) {
      break;
    }
    char_budget -= line_size;
    selected.push_back(message);
  }
  std::reverse(selected.begin(), selected.end());
  return selected;
}

Roe<std::optional<ConversationSummary>> JsonThreadStore::GetThreadMemory(const std::string& thread_id) const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  const auto it = memory_.find(thread_id);
  if (it == memory_.end()) {
    return std::optional<ConversationSummary>{};
  }
  return std::optional<ConversationSummary>{it->second};
}

Roe<void> JsonThreadStore::SetThreadMemory(const std::string& thread_id, const ConversationSummary& summary) {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  memory_[thread_id] = summary;
  return {};
}

Roe<void> JsonThreadStore::ClearThreadMemory(const std::string& thread_id) {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  memory_.erase(thread_id);
  return {};
}

Roe<int64_t> JsonThreadStore::CountContextEligibleMessagesAfter(const std::string& thread_id,
                                                                const int64_t after_display_order) const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  const auto it = messages_.find(thread_id);
  if (it == messages_.end()) {
    return int64_t{0};
  }
  int64_t count = 0;
  for (const ThreadMessage& message : it->second) {
    if (message.content_type != ChatContentType::Text && message.content_type != ChatContentType::System) {
      continue;
    }
    if (message.display_order > after_display_order) {
      ++count;
    }
  }
  return count;
}

Roe<int64_t> JsonThreadStore::CountAnnotationsForTarget(const std::string& thread_id,
                                                        const std::string& target_message_id) const {
  if (target_message_id.empty()) {
    return int64_t{0};
  }
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  const auto it = messages_.find(thread_id);
  if (it == messages_.end()) {
    return int64_t{0};
  }
  int64_t count = 0;
  for (const ThreadMessage& message : it->second) {
    if (message.content_type != ChatContentType::Annotation) {
      continue;
    }
    if (message.target_message_id && *message.target_message_id == target_message_id) {
      ++count;
    }
  }
  return count;
}

Roe<std::vector<ThreadMessage>> JsonThreadStore::GetContextEligibleMessagesAfter(
    const std::string& thread_id, const int64_t after_display_order) const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  const auto it = messages_.find(thread_id);
  if (it == messages_.end()) {
    return std::vector<ThreadMessage>{};
  }
  std::vector<ThreadMessage> eligible;
  for (const ThreadMessage& message : it->second) {
    if (message.content_type != ChatContentType::Text && message.content_type != ChatContentType::System) {
      continue;
    }
    if (message.display_order <= after_display_order) {
      continue;
    }
    eligible.push_back(message);
  }
  std::sort(eligible.begin(), eligible.end(),
            [](const ThreadMessage& a, const ThreadMessage& b) { return a.display_order < b.display_order; });
  return eligible;
}

Roe<void> JsonThreadStore::ClearMessages(const std::string& thread_id, const ClearMessagesOptions& options) {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  messages_[thread_id].clear();
  if (options.forget_memory) {
    memory_.erase(thread_id);
  }
  dirty_ = true;
  if (SaveMessages(thread_id)) {
    for (Thread& thread : threads_) {
      if (thread.id == thread_id) {
        thread.preview.clear();
        thread.unread_count = 0;
        break;
      }
    }
    (void)SaveIndex();
    return {};
  }
  return Error("Failed to clear messages");
}

Roe<std::vector<std::pair<std::string, std::string>>> JsonThreadStore::ListPendingOutbox() const {
  return std::vector<std::pair<std::string, std::string>>{};
}

Roe<bool> JsonThreadStore::UpdateMessage(const ThreadMessage& message) {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }

  auto it = messages_.find(message.thread_id);
  if (it == messages_.end()) {
    return false;
  }

  for (ThreadMessage& existing : it->second) {
    if (existing.id == message.id) {
      existing = message;
      dirty_ = true;
      if (SaveMessages(message.thread_id)) {
        return true;
      }
      return false;
    }
  }
  return false;
}

Roe<bool> JsonThreadStore::HasMessageId(const std::string& thread_id, const std::string& message_id) const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  const auto it = messages_.find(thread_id);
  if (it == messages_.end()) {
    return false;
  }
  for (const ThreadMessage& message : it->second) {
    if (message.id == message_id) {
      return true;
    }
  }
  return false;
}

Roe<bool> JsonThreadStore::DeleteThread(const std::string& thread_id) {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }

  auto thread_it = std::find_if(threads_.begin(), threads_.end(),
                                [&](const Thread& thread) { return thread.id == thread_id; });
  if (thread_it == threads_.end()) {
    return false;
  }

  const auto message_it = messages_.find(thread_id);
  if (message_it != messages_.end()) {
    messages_.erase(message_it);
  }

  threads_.erase(thread_it);

  std::error_code ec;
  std::filesystem::remove(ThreadPath(thread_id), ec);

  dirty_ = true;
  if (auto save = SaveIndex()) {
    dirty_ = false;
    return true;
  }
  return Error("Failed to save thread index after delete");
}

Roe<std::optional<Thread>> JsonThreadStore::FindDirectThread(const DirectChatTarget& target) const {
  std::lock_guard lock(mutex_);
  if (auto load = EnsureLoaded(); !load) {
    return load.error();
  }
  for (const Thread& thread : threads_) {
    if (thread.kind == ThreadKind::Direct && thread.channel == target.channel &&
        thread.peer_identity_kind == target.peer_identity_kind &&
        thread.peer_identity_value == target.peer_identity_value) {
      return Roe<std::optional<Thread>>(thread);
    }
  }
  return Roe<std::optional<Thread>>(std::optional<Thread>{});
}

Roe<Thread> JsonThreadStore::FindOrCreateDirectThread(const DirectChatTarget& target,
                                                      const std::string& participant_contact_id,
                                                      const std::string& title) {
  if (auto existing = FindDirectThread(target)) {
    if (!existing) {
      return existing.error();
    }
    if (*existing) {
      Thread thread = **existing;
      if (!participant_contact_id.empty() && thread.participant_contact_ids.empty()) {
        thread.participant_contact_ids = {participant_contact_id};
        if (!title.empty()) {
          thread.title = title;
        }
        thread.updated_at = util::NowUnixMs();
        return UpsertThread(thread);
      }
      return thread;
    }
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
  return UpsertThread(thread);
}

Roe<std::optional<Thread>> JsonThreadStore::FindGroupThread(const std::string& group_id) const {
  if (auto load = EnsureLoaded(); !load) {
    return load.error();
  }
  std::lock_guard lock(mutex_);
  for (const Thread& thread : threads_) {
    if (thread.group_id && *thread.group_id == group_id) {
      return Roe<std::optional<Thread>>(thread);
    }
  }
  return Roe<std::optional<Thread>>(std::optional<Thread>{});
}

Roe<Thread> JsonThreadStore::FindOrCreateGroupThread(const std::string& group_id, const std::string& title,
                                                     const std::vector<std::string>& participant_contact_ids) {
  if (auto existing = FindGroupThread(group_id)) {
    if (!existing) {
      return existing.error();
    }
    if (*existing) {
      return **existing;
    }
  }
  Thread thread;
  thread.id = util::GenerateUuid();
  thread.kind = ThreadKind::Group;
  thread.group_id = group_id;
  thread.participant_contact_ids = participant_contact_ids;
  thread.title = title;
  thread.encrypted = true;
  thread.updated_at = util::NowUnixMs();
  return UpsertThread(thread);
}

Roe<std::vector<ThreadMessage>> JsonThreadStore::ExportMessagesUpTo(
    const std::string& thread_id, const std::optional<std::string>& max_message_id) const {
  return GetMessages(thread_id);
}

Roe<uint64_t> JsonThreadStore::AllocateSenderSeq(const std::string& /*thread_id*/) {
  static uint64_t next_seq = 1;
  return next_seq++;
}

Roe<uint32_t> JsonThreadStore::GetChatTargetSessionEpoch(const std::string& /*thread_id*/) const {
  return 1u;
}

Roe<std::vector<ThreadMessage>> JsonThreadStore::GetMessagesBySeqRange(const std::string& thread_id,
                                                                       const SeqRangeQuery& query) const {
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  const auto it = messages_.find(thread_id);
  if (it == messages_.end()) {
    return std::vector<ThreadMessage>{};
  }

  std::vector<ThreadMessage> matched;
  for (const ThreadMessage& message : it->second) {
    if (!message.relay_visible || !message.sender_seq || !message.session_epoch) {
      continue;
    }
    if (*message.session_epoch != query.session_epoch) {
      continue;
    }
    if (message.sender_contact_id != query.seq_owner_contact_id) {
      continue;
    }
    if (query.min_sender_seq && *message.sender_seq < *query.min_sender_seq) {
      continue;
    }
    if (query.max_sender_seq && *message.sender_seq > *query.max_sender_seq) {
      continue;
    }
    matched.push_back(message);
  }

  std::sort(matched.begin(), matched.end(), [query](const ThreadMessage& a, const ThreadMessage& b) {
    return query.ascending ? *a.sender_seq < *b.sender_seq : *a.sender_seq > *b.sender_seq;
  });
  if (matched.size() > query.limit) {
    matched.resize(query.limit);
  }
  return matched;
}

Roe<PeerSyncState> JsonThreadStore::GetPeerSyncState(const std::string& /*thread_id*/,
                                                     uint32_t /*session_epoch*/) const {
  return DefaultPeerSyncState();
}

Roe<void> JsonThreadStore::SetPeerSyncState(const std::string& /*thread_id*/, uint32_t /*session_epoch*/,
                                            const PeerSyncState& /*state*/) {
  return {};
}

Roe<void> JsonThreadStore::CancelOldEpochPending(const std::string& /*thread_id*/, uint32_t /*old_session_epoch*/) {
  return {};
}

Roe<void> JsonThreadStore::AdoptChatTargetEpoch(const std::string& /*thread_id*/, uint32_t /*new_session_epoch*/) {
  return {};
}

Roe<ThreadMessage> JsonThreadStore::AppendMessageWithPassiveEpochAdopt(const ThreadMessage& message,
                                                                     uint32_t /*old_session_epoch*/,
                                                                     uint32_t /*new_session_epoch*/,
                                                                     const PeerSyncState& /*new_sync_state*/) {
  return AppendMessage(message);
}

Roe<uint32_t> JsonThreadStore::BumpLocalChatTargetEpoch(const std::string& /*thread_id*/) {
  return 2u;
}

Roe<void> JsonThreadStore::ReconcileOutbox() {
  return {};
}

} // namespace pbr
