#pragma once

#include <functional>
#include <string>

#include "lib/calls/CallScreen.h"

namespace pbr {

/** What a call button does when it is pressed while this device may already be in a call. */
enum class NewCallVerdict {
  /** Not in a call (or a ring / answer is on screen): place the call. */
  Place,
  /** The pressed chat is the chat of the call already up: no second call, bring the call screen back. */
  ShowCurrent,
  /** Another chat's call is up: ask "This will end your current call", then leave it and place the new one. */
  AskThenReplace,
};

/** Pure: `screen` is what the call screen shows now, `thread_id` the chat whose call button was pressed. */
NewCallVerdict DecideNewCall(const CallScreen& screen, const std::string& thread_id);

/** The effects of a new-call press; CallPresenter fills them in. */
struct NewCallSteps {
  /** Shows the confirmation; calls its argument with the user's answer (true = End and call). */
  std::function<void(std::function<void(bool)> answer)> ask;
  std::function<void()> show_current;
  std::function<void()> leave_current;
  std::function<void()> place;
  std::function<void()> cancelled;
};

/** Runs `verdict`: Place places; ShowCurrent shows; Ask asks, then (yes) leaves and places, (no) cancels. */
void RunNewCall(NewCallVerdict verdict, const NewCallSteps& steps);

} // namespace pbr
