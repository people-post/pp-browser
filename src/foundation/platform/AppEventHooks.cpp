#include "foundation/platform/AppEventHooks.h"

namespace pbr {

namespace {

AppEventHooks g_hooks;
bool g_key_down_is_repeat = false;

} // namespace

void SetAppEventHooks(AppEventHooks hooks) {
  g_hooks = std::move(hooks);
}

const AppEventHooks& GetAppEventHooks() {
  return g_hooks;
}

void SetKeyDownIsRepeat(const bool repeat) {
  g_key_down_is_repeat = repeat;
}

bool KeyDownIsRepeat() {
  return g_key_down_is_repeat;
}

} // namespace pbr
