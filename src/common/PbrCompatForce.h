#pragma once

/**
 * Slim pp → pbr bridge force-included on every TU that links `pp_common`.
 *
 * Intentionally excludes Value/Object/Json: those pull FiFoMap + variant into
 * every translation unit and have broken MSVC builds. Call sites that need
 * document types include `common/PbrCompat.h` or `common/ValueJson.h`.
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
