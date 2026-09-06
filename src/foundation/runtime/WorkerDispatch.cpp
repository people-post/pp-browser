#include "foundation/runtime/WorkerDispatch.h"

#include <cassert>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

WorkerPool* g_pool = nullptr;
bool g_ever_installed = false;

} // namespace

void WorkerDispatch::Install(WorkerPool* pool) {
  g_pool = pool;
  g_ever_installed = true;
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
    // Before Initialize: programming error. After Uninstall: expected (shutdown race).
    if (!g_ever_installed) {
      assert(false && "WorkerDispatch not installed (AppRuntime::Initialize)");
    }
    return;
  }
  g_pool->Post(lane, std::move(task));
}

} // namespace pbr
