#pragma once

#include "common/PbrCompat.h"

namespace pbr {

/** Why a people lookup / search failed, for choosing what to tell the user. */
enum class PeopleLookupFailure {
  NotFound,             // the directory answered: no such account / relay user
  DirectoryUnreachable, // transport failure (DNS, connect, timeout, TLS) or a 5xx from the directory
  Other,                // anything else (4xx other than 404, not configured, bad reply)
};

/**
 * Classifies a directory error: Network-category errors and "... failed with status 5xx" are DirectoryUnreachable,
 * "Account not found" / "Relay user not found" are NotFound, everything else is Other.
 */
PeopleLookupFailure ClassifyPeopleLookupError(const Error& error);

} // namespace pbr
