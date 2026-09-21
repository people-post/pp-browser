#pragma once

#include <string>
#include <vector>

namespace pbr::os {

/** OS-preferred UI locales (BCP-47-ish tags). Empty if unknown. */
std::vector<std::string> PreferredSystemLocales();

} // namespace pbr::os
