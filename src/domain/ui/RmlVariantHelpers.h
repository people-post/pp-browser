#pragma once

#include <ui/data/DataModelHandle.h>
#include <ui/base/Types.h>

#include <optional>
#include <string>

namespace pbr {

/** Coerce a Rml data-model event arg to int (INT/INT64/FLOAT/DOUBLE/STRING). */
std::optional<int> EventArgAsInt(const ui::VariantList& args, size_t index = 0);

/** Return a STRING event arg, or nullopt if missing/wrong type. */
std::optional<std::string> EventArgAsString(const ui::VariantList& args, size_t index = 0);

} // namespace pbr
