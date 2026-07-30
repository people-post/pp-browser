#include "base/media/VideoCodecOs.h"
#include "base/media/VideoCodecUnavailable.h"

#if defined(__ANDROID__)

namespace pbr {

std::unique_ptr<IVideoCodec> CreateAndroidVideoCodec() {
  // a3: MediaCodec path lands with Android dogfood; stub keeps builds green.
  return MakeUnavailableVideoCodec("Android MediaCodec H264 not wired yet");
}

} // namespace pbr

#endif
