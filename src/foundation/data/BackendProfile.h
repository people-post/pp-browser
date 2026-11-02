#pragma once

#include "foundation/data/Config.h"

#include <string>

namespace pbr {

/** Trimmed, scheme and host lower-cased, no trailing `/`: the same backend always yields the same text. */
std::string NormalizeBackendUrl(const std::string& url);

/**
 * Filesystem-safe key naming the backend this process runs against, also the profile id created for it:
 * `prod`, `qa`, or `custom-<12 hex>` (hash of the normalized registration URL, or of the origin when
 * PP_BROWSER_ORIGIN is set).
 */
std::string BackendProfileKey(const AppConfig& config);

} // namespace pbr
