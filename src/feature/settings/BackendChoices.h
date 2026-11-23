#pragma once

#include <string_view>
#include <vector>

namespace pbr {

/**
 * The services the "Service" row offers, in list order. A store build (`prod_available`): "prod" and "custom" always,
 * "staging" / "dev" only when the build has that address and the tester options are on. A tester build (no prod,
 * release-compatibility RC022): never "prod"; "staging" / "dev" whenever the build has that address (the tester
 * options guard prod, which it has not), and "custom". Pure: it never decides which service the app runs on.
 */
std::vector<const char*> BackendChoicesOffered(bool prod_available, bool staging_compiled, bool dev_compiled,
                                               bool tester_options);

/**
 * Whether picking `picked` while `current` is selected asks to confirm: only a different environment does (a saved
 * `qa` is `staging`).
 */
bool BackendSwitchNeedsConfirm(std::string_view current, std::string_view picked);

} // namespace pbr
