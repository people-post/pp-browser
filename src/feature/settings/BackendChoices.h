#pragma once

#include <vector>

namespace pbr {

/**
 * The services the "Service" row offers, in list order: "prod" and "custom" always; "qa" only when this build has a QA
 * address and the tester options are on. Pure: it never decides which service the app runs on.
 */
std::vector<const char*> BackendChoicesOffered(bool qa_compiled, bool tester);

} // namespace pbr
