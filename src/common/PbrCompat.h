#pragma once

/**
 * pp-browser bridge: pp-cpp-common owns namespace `pp`. App/base/feature code
 * remains in `pbr`. This header is force-included via the `pp_common` CMake
 * target so qualified and unqualified `pbr::` lookups for common types keep
 * working without rewriting call sites.
 *
 * Do not use `using namespace ::pp` here — that does not create `pbr::Name`.
 */

#include "common/BinaryPack.hpp"
#include "common/CivilTime.h"
#include "common/Error.h"
#include "common/Logger.h"
#include "common/Module.h"
#include "common/ResultOrError.hpp"
#include "common/SequencedTaskRunner.h"
#include "common/Serialize.hpp"
#include "common/Utilities.h"
#include "common/Value.h"
#include "common/WorkerPool.h"
#include "common/io/Json.h"

namespace pbr {

using ::pp::RoeErrorBase;
using ::pp::ResultOrError;
using ::pp::Error;
using ::pp::Roe;
using ::pp::Module;
using ::pp::WorkerLane;
using ::pp::WorkerPool;
using ::pp::SequencedTaskRunner;
using ::pp::WireLenUtf8;
using ::pp::WireLenBytes;
using ::pp::OutputArchive;
using ::pp::InputArchive;
using ::pp::binaryPack;
using ::pp::binaryUnpack;

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

namespace logging = ::pp::logging;
namespace util = ::pp::util;
namespace civil_time = ::pp::civil_time;
namespace json_io = ::pp::common::io;

} // namespace pbr
