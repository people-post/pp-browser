#include "base/media/IVideoCodec.h"
#include "base/media/VideoCodecOs.h"
#include "base/media/VideoCodecUnavailable.h"

#if defined(__linux__) && !defined(__ANDROID__)
#elif defined(__ANDROID__)
#elif defined(_WIN32)
#elif defined(__APPLE__)
#else
#endif

// Factory selects OS entry; avoid calling undeclared Create* on wrong OS.
#if defined(_WIN32)
#define PP_CREATE_OS_VIDEO_CODEC CreateWin32VideoCodec
#elif defined(__APPLE__)
#define PP_CREATE_OS_VIDEO_CODEC CreateDarwinVideoCodec
#elif defined(__ANDROID__)
#define PP_CREATE_OS_VIDEO_CODEC CreateAndroidVideoCodec
#elif defined(__linux__)
#define PP_CREATE_OS_VIDEO_CODEC CreateLinuxVideoCodec
#endif

namespace pbr {

std::unique_ptr<IVideoCodec> CreatePlatformVideoCodec() {
#ifdef PP_CREATE_OS_VIDEO_CODEC
  return PP_CREATE_OS_VIDEO_CODEC();
#else
  return MakeUnavailableVideoCodec("unsupported platform");
#endif
}

} // namespace pbr
