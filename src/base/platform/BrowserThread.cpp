#include "base/platform/BrowserThread.h"

#include "base/platform/PlatformRuntime.h"

#include <cassert>
#include <mutex>

namespace pbr {

std::unique_ptr<SequencedTaskRunner> BrowserThread::ui_runner_;
std::function<void()> BrowserThread::ui_wake_callback_;

void BrowserThread::Initialize() {
  static std::mutex init_mutex;
  std::lock_guard lock(init_mutex);
  if (!ui_runner_) {
    ui_runner_ = std::make_unique<SequencedTaskRunner>();
  }
}

void BrowserThread::Shutdown() {
  ui_wake_callback_ = nullptr;
  if (ui_runner_) {
    ui_runner_->Stop();
    ui_runner_.reset();
  }
}

SequencedTaskRunner& BrowserThread::Get(const BrowserThreadId id) {
  Initialize();
  assert(id == BrowserThreadId::UI && "BrowserThread::IO retired — use worker pool via PostTask(IO)");
  return *ui_runner_;
}

void BrowserThread::RunUITasks() {
  if (ui_runner_) {
    ui_runner_->RunPendingTasks();
  }
}

bool BrowserThread::CurrentlyOn(const BrowserThreadId id) {
  if (id == BrowserThreadId::IO) {
    return false;
  }
  return Get(id).IsRunningOnThisThread();
}

void BrowserThread::PostTask(const BrowserThreadId id, std::function<void()> task) {
  if (!task) {
    return;
  }
  if (id == BrowserThreadId::UI) {
    Get(BrowserThreadId::UI).PostTask(std::move(task));
    if (ui_wake_callback_) {
      ui_wake_callback_();
    }
    return;
  }
  PlatformRuntime::PostWorkerNormal(std::move(task));
}

void BrowserThread::PostTaskFront(const BrowserThreadId id, std::function<void()> task) {
  if (!task) {
    return;
  }
  if (id == BrowserThreadId::UI) {
    Get(BrowserThreadId::UI).PostTaskFront(std::move(task));
    if (ui_wake_callback_) {
      ui_wake_callback_();
    }
    return;
  }
  PlatformRuntime::PostWorkerCritical(std::move(task));
}

void BrowserThread::SetUIWakeCallback(std::function<void()> callback) {
  ui_wake_callback_ = std::move(callback);
}

void BrowserThread::PauseIO() {
  PlatformRuntime::PauseBackgroundWork();
}

void BrowserThread::ResumeIO() {
  PlatformRuntime::ResumeBackgroundWork();
}

} // namespace pbr
