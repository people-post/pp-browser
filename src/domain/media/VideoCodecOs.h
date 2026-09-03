#pragma once

#include "domain/media/IVideoCodec.h"

#include <memory>

namespace pbr {

/** OS-selected HW codec entry (CMake source-selects one VideoCodec_*.cpp). */
std::unique_ptr<IVideoCodec> CreateOsVideoCodec();

} // namespace pbr
