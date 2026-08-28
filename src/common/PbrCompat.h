#pragma once

/**
 * Full pp → pbr bridge including Value/Object/Json aliases.
 *
 * Prefer this (or `common/ValueJson.h`) for explicit includes. The CMake
 * force-include uses `PbrCompatForce.h` only — see that header.
 */

#include "common/PbrCompatForce.h"
#include "common/Value.h"
#include "common/io/Json.h"

namespace pbr {

using ::pp::common::Array;
using ::pp::common::ArrayPtr;
using ::pp::common::Null;
using ::pp::common::Object;
using ::pp::common::ObjectPtr;
using ::pp::common::Value;
using ::pp::common::asArray;
using ::pp::common::asNonNegInt;
using ::pp::common::asObject;
using ::pp::common::asString;
using ::pp::common::isArrayValue;
using ::pp::common::isBoolValue;
using ::pp::common::isNullValue;
using ::pp::common::isObjectValue;
using ::pp::common::isStringValue;
using ::pp::common::makeArray;
using ::pp::common::valueEqual;

namespace json_io = ::pp::common::io;

} // namespace pbr
