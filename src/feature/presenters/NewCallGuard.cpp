#include "feature/presenters/NewCallGuard.h"

namespace pbr {

NewCallVerdict DecideNewCall(const CallScreen& screen, const std::string& thread_id) {
  if (screen.kind != CallScreen::Kind::InCall) {
    return NewCallVerdict::Place;
  }
  if (!thread_id.empty() && screen.thread_id == thread_id) {
    return NewCallVerdict::ShowCurrent;
  }
  return NewCallVerdict::AskThenReplace;
}

void RunNewCall(const NewCallVerdict verdict, const NewCallSteps& steps) {
  switch (verdict) {
  case NewCallVerdict::Place:
    if (steps.place) {
      steps.place();
    }
    return;
  case NewCallVerdict::ShowCurrent:
    if (steps.show_current) {
      steps.show_current();
    }
    return;
  case NewCallVerdict::AskThenReplace:
    if (!steps.ask) {
      if (steps.cancelled) {
        steps.cancelled();
      }
      return;
    }
    steps.ask([steps](const bool yes) {
      if (!yes) {
        if (steps.cancelled) {
          steps.cancelled();
        }
        return;
      }
      if (steps.leave_current) {
        steps.leave_current();
      }
      if (steps.place) {
        steps.place();
      }
    });
    return;
  }
}

} // namespace pbr
