#include "base/runtime/AppRuntime.h"

#include "common/SequencedTaskRunner.h"

#include <mutex>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::unique_ptr<SequencedTaskRunner> g_ui_runner;
std::function<void()> g_ui_wake_callback;

void EnsureUIMailbox() {
  static std::mutex init_mutex;
  std::lock_guard lock(init_mutex);
  if (!g_ui_runner) {
    g_ui_runner = std::make_unique<SequencedTaskRunner>();
  }
}

} // namespace

void AppRuntime::InitializeUI() {
  EnsureUIMailbox();
}

void AppRuntime::ShutdownUI() {
  g_ui_wake_callback = nullptr;
  if (g_ui_runner) {
    g_ui_runner->Stop();
    g_ui_runner.reset();
  }
}

void AppRuntime::PostUI(std::function<void()> task) {
  if (!task) {
    return;
  }
  EnsureUIMailbox();
  g_ui_runner->PostTask(std::move(task));
  if (g_ui_wake_callback) {
    g_ui_wake_callback();
  }
}

void AppRuntime::PostUIFront(std::function<void()> task) {
  if (!task) {
    return;
  }
  EnsureUIMailbox();
  g_ui_runner->PostTaskFront(std::move(task));
  if (g_ui_wake_callback) {
    g_ui_wake_callback();
  }
}

void AppRuntime::RunUITasks() {
  if (g_ui_runner) {
    g_ui_runner->RunPendingTasks();
  }
}

bool AppRuntime::HasPendingUITasks() {
  EnsureUIMailbox();
  return g_ui_runner && g_ui_runner->HasPendingTasks();
}

bool AppRuntime::CurrentlyOnUI() {
  EnsureUIMailbox();
  return g_ui_runner && g_ui_runner->IsRunningOnThisThread();
}

void AppRuntime::SetUIWakeCallback(std::function<void()> callback) {
  EnsureUIMailbox();
  g_ui_wake_callback = std::move(callback);
}

} // namespace pbr
