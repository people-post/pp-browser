#pragma once

/**
 * Transitional pp-browser bridge: pp-cpp-common owns namespace `pp`. App/base/
 * feature code remains in `pbr`. Include this header explicitly in TUs that
 * still rely on unqualified Roe/Object/Module/etc. inside namespace pbr.
 *
 * New code should prefer `pp::` types directly; JSON helpers live in
 * ValueJson.h without this header.
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
#include "common/AtomicFileWrite.h"
#include "common/CodedFailure.h"
#include "common/SettledWait.h"
#include "common/ValueJson.h"
#include "common/metrics/MetricsRegistry.h"

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

// JSON helpers (pp-cpp-common ValueJson).
using ::pp::common::ArrayValue;
using ::pp::common::DeepMergeObject;
using ::pp::common::DumpJson;
using ::pp::common::ObjectArray;
using ::pp::common::ObjectBool;
using ::pp::common::ObjectChild;
using ::pp::common::ObjectInt64;
using ::pp::common::ObjectNonNegInt;
using ::pp::common::ObjectString;
using ::pp::common::ObjectValue;
using ::pp::common::ParseObject;
using ::pp::common::ParseValue;
using ::pp::common::TryParseObject;

using ::pp::AtomicFileWrite;
using ::pp::CodedFailure;
using ::pp::CodedRoe;
using ::pp::SettledWait;
namespace detail {
using ::pp::detail::AppendFrom;
} // namespace detail

using ::pp::FixedLabelCounts;
using ::pp::MetricCounter;
using ::pp::MetricGauge;
using ::pp::MetricHistogram;
using ::pp::MetricLabelKey;
using ::pp::MetricLabels;
using ::pp::MetricsRegistry;
using ::pp::ScopedMetricsCollector;

namespace logging = ::pp::logging;
namespace util = ::pp::util;
namespace civil_time = ::pp::civil_time;
namespace json_io = ::pp::common::io;

} // namespace pbr
