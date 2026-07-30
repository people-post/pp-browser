#include "base/media/VideoCodecOs.h"
#include "base/media/VideoCodecUnavailable.h"

#if defined(__linux__) && !defined(__ANDROID__)

namespace pbr {

std::unique_ptr<IVideoCodec> CreateLinuxVideoCodec() {
  // V017: VA-API / V4L2 M2M best-effort — not wired yet. Voice stays up (V019).
  return MakeUnavailableVideoCodec("Linux VA-API H264 not wired yet");
}

} // namespace pbr

#endif
