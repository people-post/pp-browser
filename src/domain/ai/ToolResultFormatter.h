#pragma once

#include <string>

namespace pbr {

std::string FormatToolResultForLlm(const std::string& tool_name, const std::string& raw_result);

} // namespace pbr
