#pragma once

#include "base/media/IVideoCodec.h"

#include <memory>

namespace pbr {

std::unique_ptr<IVideoCodec> CreateWin32VideoCodec();
std::unique_ptr<IVideoCodec> CreateDarwinVideoCodec();
std::unique_ptr<IVideoCodec> CreateLinuxVideoCodec();
std::unique_ptr<IVideoCodec> CreateAndroidVideoCodec();

} // namespace pbr
