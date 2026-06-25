#include "platform/BrowserThread.h"

namespace pbr {

std::unique_ptr<SequencedTaskRunner> BrowserThread::ui_runner_;
std::unique_ptr<SequencedTaskRunner> BrowserThread::io_runner_;

void BrowserThread::Initialize() {
  if (!ui_runner_) {
    ui_runner_ = std::make_unique<SequencedTaskRunner>(false);
  }
  if (!io_runner_) {
    io_runner_ = std::make_unique<SequencedTaskRunner>(true);
  }
}

void BrowserThread::Shutdown() {
  if (io_runner_) {
    io_runner_->Stop();
    io_runner_.reset();
  }
  if (ui_runner_) {
    ui_runner_->Stop();
    ui_runner_.reset();
  }
}

SequencedTaskRunner& BrowserThread::Get(const BrowserThreadId id) {
  Initialize();
  return id == BrowserThreadId::IO ? *io_runner_ : *ui_runner_;
}

void BrowserThread::RunUITasks() {
  if (ui_runner_) {
    ui_runner_->RunPendingTasks();
  }
}

bool BrowserThread::CurrentlyOn(const BrowserThreadId id) {
  return Get(id).IsRunningOnThisThread();
}

void BrowserThread::PostTask(const BrowserThreadId id, std::function<void()> task) {
  Get(id).PostTask(std::move(task));
}

void BrowserThread::PauseIO() {
  if (io_runner_) {
    io_runner_->Pause();
  }
}

void BrowserThread::ResumeIO() {
  if (io_runner_) {
    io_runner_->Resume();
  }
}

} // namespace pbr
