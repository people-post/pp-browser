#include "foundation/runtime/OwnerThread.h"

#include "common/Logger.h"
#include "common/metrics/MetricsRegistry.h"

#include <exception>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

thread_local const OwnerThread* t_current_owner = nullptr;

logging::Logger& OwnerLog() {
  static logging::Logger log = logging::getLogger("Runtime.OwnerThread");
  return log;
}

} // namespace

const char* OwnerThreadName(OwnerThreadId id) {
  switch (id) {
  case OwnerThreadId::MediaSessions:
    return "pp-media-sess";
  case OwnerThreadId::Connectivity:
    return "pp-connectivity";
  }
  return "pp-owner";
}

OwnerThread::OwnerThread(std::string name, OwnerThreadMode mode, std::function<void(const std::string&)> name_thread)
    : name_(std::move(name)), mode_(mode), name_thread_(std::move(name_thread)) {
  static const std::vector<double> kBounds{0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1, 5};
  MetricsRegistry& registry = MetricsRegistry::Global();
  wait_seconds_ = &registry.Histogram("pp_runtime_task_wait_seconds", "Owner thread task time queued before it ran.",
                                      kBounds, {{"owner", name_}});
  run_seconds_ = &registry.Histogram("pp_runtime_task_run_seconds", "Owner thread task run time.", kBounds,
                                     {{"owner", name_}});
}

OwnerThread::~OwnerThread() {
  Stop();
}

void OwnerThread::Start() {
  std::lock_guard lock(mu_);
  if (started_ || stopped_) {
    return;
  }
  started_ = true;
  if (mode_ == OwnerThreadMode::Dedicated) {
    thread_ = std::thread([this]() { ThreadMain(); });
  }
}

void OwnerThread::Stop() {
  std::deque<Queued> dropped;
  {
    std::lock_guard lock(mu_);
    if (stopped_) {
      return;
    }
    stopped_ = true;
    dropped.swap(tasks_);
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    if (thread_.get_id() == std::this_thread::get_id()) {
      thread_.detach();  // Stop from one of our own tasks: the loop exits after it returns
    } else {
      thread_.join();
    }
  }
  // `dropped` is destroyed here, outside the lock (gated tasks settle their teardown count).
}

bool OwnerThread::Post(std::function<void()> task) {
  if (!task) {
    return false;
  }
  {
    std::lock_guard lock(mu_);
    if (stopped_) {
      return false;
    }
    tasks_.push_back(Queued{std::move(task), std::chrono::steady_clock::now()});
  }
  cv_.notify_one();
  return true;
}

bool OwnerThread::PostFront(std::function<void()> task) {
  if (!task) {
    return false;
  }
  {
    std::lock_guard lock(mu_);
    if (stopped_) {
      return false;
    }
    tasks_.push_front(Queued{std::move(task), std::chrono::steady_clock::now()});
  }
  cv_.notify_one();
  return true;
}

bool OwnerThread::IsCurrent() const {
  return t_current_owner == this;
}

bool OwnerThread::HasPending() const {
  std::lock_guard lock(mu_);
  return !tasks_.empty();
}

size_t OwnerThread::QueueDepth() const {
  std::lock_guard lock(mu_);
  return tasks_.size();
}

size_t OwnerThread::RunPending() {
  size_t ran = 0;
  for (;;) {
    Queued task;
    {
      std::lock_guard lock(mu_);
      if (stopped_ || tasks_.empty()) {
        return ran;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    const OwnerThread* outer = t_current_owner;
    t_current_owner = this;
    RunTask(task);
    t_current_owner = outer;
    ++ran;
  }
}

void OwnerThread::ThreadMain() {
  if (name_thread_) {
    name_thread_(name_);
  }
  t_current_owner = this;
  for (;;) {
    Queued task;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [this]() { return stopped_ || !tasks_.empty(); });
      if (stopped_) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    RunTask(task);
  }
}

void OwnerThread::RunTask(Queued& queued) {
  using Seconds = std::chrono::duration<double>;
  const auto started = std::chrono::steady_clock::now();
  wait_seconds_->Observe(Seconds(started - queued.enqueued).count());
  std::function<void()>& task = queued.task;
  try {
    task();
  } catch (const std::exception& e) {
    OwnerLog().error << name_ << " task threw: " << e.what();
  } catch (...) {
    OwnerLog().error << name_ << " task threw a non-std exception";
  }
  task = nullptr;  // destroy captures on the owner
  run_seconds_->Observe(Seconds(std::chrono::steady_clock::now() - started).count());
}

} // namespace pbr
