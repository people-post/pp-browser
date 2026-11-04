#pragma once

#include "common/Error.h"
#include "common/thread/ThreadMemoryTypes.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class IThreadMemory {
public:
  virtual ~IThreadMemory() = default;

  virtual Roe<std::optional<ConversationSummary>> GetThreadMemory(const std::string& thread_id) const = 0;
  virtual Roe<void> SetThreadMemory(const std::string& thread_id, const ConversationSummary& summary) = 0;
  virtual Roe<void> ClearThreadMemory(const std::string& thread_id) = 0;
};

} // namespace pbr
