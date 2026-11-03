#include "lib/media/IVideoCodec.h"
#include "lib/media/VideoCodecOs.h"

namespace pbr {

namespace {

std::function<std::unique_ptr<IVideoCodec>()>& CodecOverride() {
  static std::function<std::unique_ptr<IVideoCodec>()> make;
  return make;
}

} // namespace

void SetPlatformVideoCodecOverride(std::function<std::unique_ptr<IVideoCodec>()> make) {
  CodecOverride() = std::move(make);
}

std::unique_ptr<IVideoCodec> CreatePlatformVideoCodec() {
  if (const auto& make = CodecOverride()) {
    return make();
  }
  return CreateOsVideoCodec();
}

bool PlatformVideoEncoderSupported() {
  if (const auto& make = CodecOverride()) {
    auto probe = make();
    return probe && probe->EncoderSupported();
  }
  static const bool supported = [] {
    auto probe = CreateOsVideoCodec();
    return probe && probe->EncoderSupported();
  }();
  return supported;
}

} // namespace pbr
