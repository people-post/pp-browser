#include "base/media/IVideoCodec.h"
#include "base/media/VideoCodecOs.h"

namespace pbr {

std::unique_ptr<IVideoCodec> CreatePlatformVideoCodec() {
  return CreateOsVideoCodec();
}

} // namespace pbr
