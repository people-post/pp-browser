#include "base/media/VideoCodecOs.h"
#include "base/media/VideoCodecUnavailable.h"

#if defined(__APPLE__)

namespace pbr {

std::unique_ptr<IVideoCodec> CreateDarwinVideoCodec() {
  // VideoToolbox path fills in with macOS dogfood; unavailable keeps cross-compiles sober.
  return MakeUnavailableVideoCodec("macOS VideoToolbox H264 not wired yet");
}

} // namespace pbr

#endif
