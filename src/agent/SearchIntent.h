#pragma once

#include <string>

namespace pbr {

// True when the user message likely needs live/web data and search should run before the LLM answers.
bool ShouldProactiveWebSearch(const std::string& user_message);

} // namespace pbr
