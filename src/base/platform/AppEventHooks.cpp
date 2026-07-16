#include "base/platform/AppEventHooks.h"

namespace pbr {

namespace {

AppEventHooks g_hooks;

} // namespace

void SetAppEventHooks(AppEventHooks hooks) {
  g_hooks = std::move(hooks);
}

const AppEventHooks& GetAppEventHooks() {
  return g_hooks;
}

} // namespace pbr
