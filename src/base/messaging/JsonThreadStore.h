#pragma once

#include "common/Module.h"
#include "base/people/ContactTypes.h"
#include "base/messaging/IThreadStore.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pbr {

class JsonThreadStore : public Module, public IThreadStore {
public:
  explicit JsonThreadStore(std::string data_dir);

  Roe<std::vector<Thread>> ListThreads() const override;
  Roe<std::optional<Thread>> GetThread(const std::string& thread_id) const override;
  Roe<Thread> UpsertThread(const Thread& thread) override;
  Roe<std::vector<ThreadMessage>> GetMessages(const std::string& thread_id) const override;
  Roe<ThreadMessage> AppendMessage(const ThreadMessage& message) override;
  Roe<bool> UpdateMessage(const ThreadMessage& message) override;
  Roe<bool> HasMessageId(const std::string& message_id) const override;
  Roe<bool> DeleteThread(const std::string& thread_id) override;
  void Flush() override;

private:
  Roe<void> EnsureLoaded() const;
  Roe<void> SaveIndex() const;
  Roe<void> SaveMessages(const std::string& thread_id) const;
  std::string ThreadPath(const std::string& thread_id) const;
  std::string IndexPath() const;

  std::string data_dir_;
  mutable std::mutex mutex_;
  mutable bool loaded_ = false;
  mutable std::vector<Thread> threads_;
  mutable std::unordered_map<std::string, std::vector<ThreadMessage>> messages_;
  mutable std::unordered_map<std::string, bool> message_ids_;
  mutable bool dirty_ = false;
};

} // namespace pbr
