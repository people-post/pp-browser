#include "feature/ai/AiTurnPool.h"

#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

AiTurnPool::AiTurnPool(const size_t threads) : pool_(threads) {}

AiTurnPool::~AiTurnPool() {
  Shutdown();
}

void AiTurnPool::Post(std::function<void()> task) {
  // Counted until the task is destroyed: after it ran, or when a stopped pool drops it.
  struct Token {
    explicit Token(std::shared_ptr<Outstanding> owner) : outstanding(std::move(owner)) {}
    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;
    ~Token() {
      std::lock_guard lock(outstanding->mu);
      --outstanding->count;
      outstanding->cv.notify_all();
    }
    std::shared_ptr<Outstanding> outstanding;
  };
  {
    std::lock_guard lock(outstanding_->mu);
    ++outstanding_->count;
  }
  pool_.Post(WorkerLane::Normal, [token = std::make_shared<Token>(outstanding_), task = std::move(task)]() {
    task();
  });
}

bool AiTurnPool::WaitIdle(const std::chrono::milliseconds budget) {
  std::unique_lock lock(outstanding_->mu);
  return outstanding_->cv.wait_for(lock, budget, [&] { return outstanding_->count == 0; });
}

void AiTurnPool::Shutdown(const std::chrono::milliseconds budget) {
  (void)pool_.Shutdown(budget);
}

} // namespace pbr
