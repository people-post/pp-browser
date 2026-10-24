#include "foundation/platform/CallSystemUi.h"

namespace pbr {

namespace {

ICallSystemUi g_null_call_system_ui;
ICallSystemUi* g_call_system_ui = nullptr;

} // namespace

ICallSystemUi& ICallSystemUi::Instance() {
  return g_call_system_ui ? *g_call_system_ui : g_null_call_system_ui;
}

void ICallSystemUi::SetInstance(ICallSystemUi* ui) {
  g_call_system_ui = ui;
}

} // namespace pbr
