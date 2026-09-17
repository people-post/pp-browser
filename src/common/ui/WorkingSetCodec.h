#pragma once

#include "common/ui/WorkingSetTypes.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** Serialize working-set candidates for transcript persistence. */
std::string WorkingSetCandidatesToJson(const std::vector<WorkingSetCandidate>& candidates);

/** Parse candidates JSON; empty vector on invalid/empty input. */
std::vector<WorkingSetCandidate> WorkingSetCandidatesFromJson(const std::string& json);

/** True when RML contains any working-set reopen chip (active or disabled). */
bool ContentRmlHasWorkingSetChip(const std::string& rml);

/** True when RML has a clickable open_working_set chip. */
bool ContentRmlHasActiveWorkingSetChip(const std::string& rml);

/**
 * When the artifact is no longer available, replace active chips with a muted,
 * non-clickable control so history still shows that results existed.
 */
std::string MarkWorkingSetChipsUnavailable(const std::string& rml);

/**
 * Remove bubble-inlined Message/Add suggestion buttons that belong in the panel.
 * Use when the reply already has (or had) a working-set chip.
 */
std::string StripInlinedWorkingSetActionSuggestions(const std::string& rml);

}  // namespace pbr
