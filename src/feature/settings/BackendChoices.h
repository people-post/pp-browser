#pragma once

#include <string_view>
#include <vector>

namespace pbr {

/**
 * The services the "Service" row offers, in list order: "prod" and "custom" always; "staging" / "dev" only when this
 * build has that address and the tester options are on. Pure: it never decides which service the app runs on.
 */
std::vector<const char*> BackendChoicesOffered(bool staging_compiled, bool dev_compiled, bool tester);

/** Whether picking `picked` while `current` is selected asks to confirm: only a different environment does (a saved `qa` is
 * `staging`). */
bool BackendSwitchNeedsConfirm(std::string_view current, std::string_view picked);

} // namespace pbr
