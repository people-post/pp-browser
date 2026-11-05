#pragma once

#include "lib/ui/ShellTypes.h"

#include <string>
#include <vector>

namespace pbr {

/**
 * Who is spotlit in Immersive (V059): one person's tile fills the call area, the others line up in a strip below.
 * The user's tap wins while that person is in the call; "back to grid" stays until another tap; otherwise a 1:1
 * call spotlights the peer and a group call its active speaker (CallActiveSpeaker), the grid until someone speaks.
 */
struct CallSpotlightChoice {
  /** The stream the user tapped (empty: none). */
  std::string stream;
  /** The user chose the grid: no spotlight until the next tap. */
  bool grid = false;
};

/** The spotlit roster row's stream_id, or empty for the grid. */
inline std::string ResolveCallSpotlight(const std::vector<CallRosterParticipantState>& roster, const bool group,
                                        const CallSpotlightChoice& choice, const std::string& active_speaker = {}) {
  auto in_roster = [&roster](const std::string& stream) {
    for (const CallRosterParticipantState& row : roster) {
      if (stream == row.stream_id.c_str()) {
        return true;
      }
    }
    return false;
  };
  if (!choice.stream.empty() && in_roster(choice.stream)) {
    return choice.stream;
  }
  if (choice.grid) {
    return {};
  }
  if (group) {
    return !active_speaker.empty() && in_roster(active_speaker) ? active_speaker : std::string{};
  }
  for (const CallRosterParticipantState& row : roster) {
    if (!row.is_local) {
      return row.stream_id.c_str();
    }
  }
  return {};
}

} // namespace pbr
