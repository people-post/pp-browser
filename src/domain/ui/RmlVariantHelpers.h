#pragma once

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

#include <optional>
#include <string>

namespace pbr {

/** Coerce a Rml data-model event arg to int (INT/INT64/FLOAT/DOUBLE/STRING). */
std::optional<int> EventArgAsInt(const Rml::VariantList& args, size_t index = 0);

/** Return a STRING event arg, or nullopt if missing/wrong type. */
std::optional<std::string> EventArgAsString(const Rml::VariantList& args, size_t index = 0);

} // namespace pbr
