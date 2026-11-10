#include "feature/presenters/CallRingSteps.h"

namespace pbr {

std::array<CallRingStep, 3> CallRingSteps(const bool awaiting_invite, const std::string& caller) {
  using State = CallRingStep::State;
  if (awaiting_invite) {
    return {CallRingStep{.key = "call.ring.step.received", .state = State::Done},
            CallRingStep{.key = "call.ring.step.joining", .state = State::Current},
            CallRingStep{.key = "call.ring.step.verifying", .state = State::Pending}};
  }
  return {CallRingStep{.key = "call.ring.step.received", .state = State::Done},
          CallRingStep{.key = "call.ring.step.joined", .state = State::Done},
          CallRingStep{.text = caller, .state = State::Done}};
}

} // namespace pbr
