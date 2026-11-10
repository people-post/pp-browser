#pragma once

#include <array>
#include <string>

namespace pbr {

/**
 * The three lines before the answer on the incoming-call page (and kept, all done, at the head of the answering
 * page's list): the call arrived, we are on the encrypted network, the caller is known. Decided here; the GUI only
 * draws them.
 */
struct CallRingStep {
  enum class State { Done, Current, Pending };
  /** Locale key for the words; empty when `text` is already the words (the caller's name). */
  std::string key;
  std::string text;
  State state = State::Pending;

  bool operator==(const CallRingStep&) const = default;
};

/**
 * Awaiting the invite (a tapped alert): [received done, joining current, verifying pending]. Once the call is there:
 * [received, joined, `caller` (a name, "Stranger", or a group's title)], all done. `caller` is shown as given.
 */
std::array<CallRingStep, 3> CallRingSteps(bool awaiting_invite, const std::string& caller);

} // namespace pbr
