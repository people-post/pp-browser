#pragma once

#include "base/media/IVideoCodec.h"

#include <memory>
#include <string>

namespace pbr {

std::unique_ptr<IVideoCodec> MakeUnavailableVideoCodec(std::string reason);

} // namespace pbr
