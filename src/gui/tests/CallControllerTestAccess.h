#pragma once

#include "feature/presenters/CallPresenter.h"
#include "gui/CallController.h"

#include <optional>

namespace pbr {

/**
 * Test seam into CallController (a friend): the presenter's screen and kept failed page it draws from, and the render
 * steps. The in-call / outgoing paths enter through `in_call_.active` + SyncShellState: the real RenderInCall needs a
 * live media backend, so those tests do not exercise it.
 */
struct CallControllerTestAccess {
  static CallScreen& Screen(CallController& c) { return c.presenter_.ScreenForTest(); }
  static std::optional<CallFailedPage>& Failed(CallController& c) { return c.presenter_.FailedPageForTest(); }
  static void Render(CallController& c) { c.Render(); }
  static void RenderAwaitingRing(CallController& c) { c.RenderAwaitingRing(); }
  /** What RenderInCall ends with, without a live media backend behind it. */
  static void InCallSync(CallController& c) {
    c.in_call_.active = true;
    c.SyncShellState();
  }
  static void EndCall(CallController& c) {
    c.in_call_ = {};
    c.ring_ = {};
    c.SyncShellState();
  }
};

} // namespace pbr
