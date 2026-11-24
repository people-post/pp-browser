#pragma once

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The words for a failed link (multi-device M027): a code that is not a link code, a reply for another device, one
 * whose time ran out, or one made by a newer app. Empty for any other error (the caller's own words).
 * `on_new_device`: the new device reading the main device's reply (else the main device reading the new one's code).
 */
std::string LinkErrorText(const Error& error, bool on_new_device);

} // namespace pbr
