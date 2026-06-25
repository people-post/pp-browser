#pragma once

#include "common/Error.h"
#include "base/messaging/ThreadTypes.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

class IThreadStore {
public:
  virtual ~IThreadStore() = default;

  virtual Roe<std::vector<Thread>> ListThreads() const = 0;
  virtual Roe<std::optional<Thread>> GetThread(const std::string& thread_id) const = 0;
  virtual Roe<Thread> UpsertThread(const Thread& thread) = 0;
  virtual Roe<std::vector<ThreadMessage>> GetMessages(const std::string& thread_id) const = 0;
  virtual Roe<ThreadMessage> AppendMessage(const ThreadMessage& message) = 0;
  virtual Roe<bool> UpdateMessage(const ThreadMessage& message) = 0;
  virtual Roe<bool> HasMessageId(const std::string& message_id) const = 0;
  virtual Roe<bool> DeleteThread(const std::string& thread_id) = 0;
  virtual void Flush() = 0;
};

} // namespace pbr
