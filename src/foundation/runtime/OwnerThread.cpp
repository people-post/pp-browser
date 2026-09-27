#include "foundation/runtime/OwnerThread.h"

#include "common/Logger.h"

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
    : name_(std::move(name)), mode_(mode), name_thread_(std::move(name_thread)) {}

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
  std::deque<std::function<void()>> dropped;
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
    tasks_.push_back(std::move(task));
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

size_t OwnerThread::RunPending() {
  size_t ran = 0;
  for (;;) {
    std::function<void()> task;
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
    std::function<void()> task;
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

void OwnerThread::RunTask(std::function<void()>& task) {
  try {
    task();
  } catch (const std::exception& e) {
    OwnerLog().error << name_ << " task threw: " << e.what();
  } catch (...) {
    OwnerLog().error << name_ << " task threw a non-std exception";
  }
  task = nullptr;  // destroy captures on the owner
}

} // namespace pbr
