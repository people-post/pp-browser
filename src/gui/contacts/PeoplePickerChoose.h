#pragma once

#include "feature/presenters/MessageForward.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace pbr {

/**
 * The people picker's "choose and return" mode (DEV/PLAN-message-forward-text.md): it lists chats and people from
 * `list`, lets up to `max_targets` be ticked, and hands the ticked ones back. It invites, calls and creates nothing.
 * The words are the caller's (the picker reads no Tr() for this mode).
 */
struct PeoplePickerChooseOptions {
  std::string title;
  /** The confirm button. */
  std::string send_label;
  /** The strip above the buttons: what is about to be sent. Empty: no strip. */
  std::string preview;
  /** The bottom pill when one more than `max_targets` is tapped. */
  std::string limit_message;
  size_t max_targets = 3;
  /** What can be chosen, in the order to show it (the list is read when the picker opens). */
  std::function<std::vector<ForwardTarget>()> list;
};

/** The chosen ones, in the list's order. Runs after the picker is closed. */
using PeoplePickerChooseDone = std::function<void(std::vector<ForwardTarget> chosen)>;

} // namespace pbr
