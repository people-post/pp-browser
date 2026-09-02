#pragma once

#include "common/Module.h"
#include "common/thread/IThreadCatalog.h"
#include "common/thread/IThreadMemory.h"
#include "common/thread/IThreadTranscript.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include "common/PbrCompat.h"

namespace pbr {

class LlmClient;

/** Async AI transcript compaction (D040). */
class ThreadCompactionService : public Module {
public:
  ThreadCompactionService(IThreadCatalog& catalog, IThreadTranscript& transcript, IThreadMemory& memory,
                          LlmClient* llm);

  void MaybeCompactAsync(const std::string& thread_id);

private:
  void RunCompaction(const std::string& thread_id);

  IThreadCatalog& catalog_;
  IThreadTranscript& transcript_;
  IThreadMemory& memory_;
  LlmClient* llm_;
  std::mutex pending_mutex_;
  std::unordered_set<std::string> pending_threads_;
  std::atomic<bool> running_{false};
};

} // namespace pbr
