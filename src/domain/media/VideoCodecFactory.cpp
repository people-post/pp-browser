#include "domain/media/IVideoCodec.h"
#include "domain/media/VideoCodecOs.h"

namespace pbr {

std::unique_ptr<IVideoCodec> CreatePlatformVideoCodec() {
  return CreateOsVideoCodec();
}

} // namespace pbr
