#pragma once

#include "foundation/runtime/OwnerTasks.h"

#include "common/PbrCompat.h"

namespace pbr {

/** The calls owner's executor: the media-sessions owner (else UI), via CallsThread. CallStack's. */
OwnerExecutor& CallsOwnerExecutor();

} // namespace pbr
