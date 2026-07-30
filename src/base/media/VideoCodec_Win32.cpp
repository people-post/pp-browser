#include "base/media/VideoCodecOs.h"
#include "base/media/VideoCodecUnavailable.h"

#if defined(_WIN32)

namespace pbr {

std::unique_ptr<IVideoCodec> CreateWin32VideoCodec() {
  // Media Foundation HW path fills in with Win dogfood; unavailable keeps cross-compiles sober.
  return MakeUnavailableVideoCodec("Windows Media Foundation H264 not wired yet");
}

} // namespace pbr

#endif
