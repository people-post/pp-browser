#include "base/messaging/JsonThreadStore.h"

#include "base/messaging/MessagingJson.h"

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

  std::ofstream out(IndexPath());
  if (!out) {
    return Error("Failed to write thread index");
  }
  out << root.dump(2);
  return {};
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

  std::ofstream out(ThreadPath(thread_id));
  if (!out) {
    return Error("Failed to write thread messages: " + thread_id);
  }
  out << root.dump(2);
  return {};
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
  return GetMessagesPage(thread_id, std::nullopt, static_cast<size_t>(budget.max_turn_pairs * 2));
}

Roe<void> JsonThreadStore::ClearMessages(const std::string& thread_id, const ClearMessagesOptions& options) {
  (void)options;
  std::lock_guard lock(mutex_);
  auto load = EnsureLoaded();
  if (!load) {
    return load.error();
  }
  messages_[thread_id].clear();
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

} // namespace pbr
