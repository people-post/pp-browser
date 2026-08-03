#include "base/runtime/AppRuntime.h"

#include "base/runtime/BrowserThread.h"

namespace pbr {

void AppRuntime::PostUI(std::function<void()> task) {
  if (!task) {
    return;
  }
  BrowserThread::PostTask(BrowserThreadId::UI, std::move(task));
}

} // namespace pbr
