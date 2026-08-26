#include "common/WorkerDispatch.h"

#include <cassert>

namespace pp {

namespace {

WorkerPool* g_pool = nullptr;

} // namespace

void WorkerDispatch::Install(WorkerPool* pool) {
  g_pool = pool;
}

void WorkerDispatch::Uninstall() {
  g_pool = nullptr;
}

bool WorkerDispatch::IsInstalled() {
  return g_pool != nullptr;
}

void WorkerDispatch::Post(WorkerLane lane, std::function<void()> task) {
  if (!task) {
    return;
  }
  if (!g_pool) {
    assert(false && "WorkerDispatch not installed (AppRuntime::Initialize)");
    return;
  }
  g_pool->Post(lane, std::move(task));
}

} // namespace pp
