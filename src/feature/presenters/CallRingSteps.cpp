#include "feature/presenters/CallRingSteps.h"

namespace pbr {

std::array<CallRingStep, 3> CallRingSteps(const bool awaiting_invite, const std::string& caller,
                                          const CallRingMedia media) {
  using State = CallRingStep::State;
  if (awaiting_invite) {
    return {CallRingStep{.key = "call.ring.step.received", .state = State::Done},
            CallRingStep{.key = "call.ring.step.joining", .state = State::Current},
            CallRingStep{.key = "call.ring.step.verifying", .state = State::Pending}};
  }
  const char* const received = media == CallRingMedia::Voice   ? "call.ring.step.received_voice"
                               : media == CallRingMedia::Video ? "call.ring.step.received_video"
                                                               : "call.ring.step.received";
  return {CallRingStep{.key = received, .state = State::Done},
          CallRingStep{.key = "call.ring.step.joined", .state = State::Done},
          CallRingStep{.text = caller, .state = State::Done}};
}

} // namespace pbr
