#include "domain/media/IVideoCodec.h"
#include "domain/media/VideoCodecOs.h"

namespace pbr {

std::unique_ptr<IVideoCodec> CreatePlatformVideoCodec() {
  return CreateOsVideoCodec();
}

bool PlatformVideoEncoderSupported() {
  static const bool supported = [] {
    auto probe = CreateOsVideoCodec();
    return probe && probe->EncoderSupported();
  }();
  return supported;
}

} // namespace pbr
