#pragma once

#include "lib/calls/CallTypes.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What a call's group path records on the call's stored row (V055: the session workflow is the rows'
 * one writer, so the path asks it). Each note changes one field of the row as it is now — never a
 * whole row read earlier. Calls owner.
 */
class ICallSessionNotes {
public:
  virtual ~ICallSessionNotes() = default;

  /** The hop the call is on (`hop_peer_id`), or none (empty): the session's `sfu_hint`. */
  virtual void NoteCallHop(const std::string& call_id, const std::string& hop_peer_id) = 0;
  /** The group's planned hop moved to `hop` (the one adjustment at a join). */
  virtual void NotePlannedHop(const std::string& call_id, const CallPlannedHop& hop) = 0;
  /** `identity` is out of the call (ejected after a failed migrate). */
  virtual void NoteParticipantLeft(const std::string& call_id, const std::string& identity) = 0;
};

} // namespace pbr
