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
#include "common/WorkerPool.h"

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

namespace logging = ::pp::logging;
namespace util = ::pp::util;
namespace civil_time = ::pp::civil_time;

} // namespace pbr
