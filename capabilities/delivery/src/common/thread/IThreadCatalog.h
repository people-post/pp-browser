#pragma once

#include "common/Error.h"
#include "common/thread/ThreadRecordTypes.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class IThreadCatalog {
public:
  virtual ~IThreadCatalog() = default;

  virtual Roe<std::vector<Thread>> ListThreads() const = 0;
  virtual Roe<std::optional<Thread>> GetThread(const std::string& thread_id) const = 0;
  virtual Roe<Thread> UpsertThread(const Thread& thread) = 0;
  virtual Roe<bool> DeleteThread(const std::string& thread_id) = 0;

  virtual Roe<std::optional<Thread>> FindDirectThread(const DirectChatTarget& target) const = 0;
  virtual Roe<Thread> FindOrCreateDirectThread(const DirectChatTarget& target,
                                               const std::string& participant_contact_id,
                                               const std::string& title) = 0;
  virtual Roe<std::optional<Thread>> FindGroupThread(const std::string& group_id) const = 0;
  virtual Roe<Thread> FindOrCreateGroupThread(const std::string& group_id, const std::string& title,
                                              const std::vector<std::string>& participant_contact_ids) = 0;
};

} // namespace pbr
