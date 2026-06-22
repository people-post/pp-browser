#include "platform/AssetIO.h"

#include "platform/Platform.h"

#include <fstream>
#include <sstream>

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#include <SDL3/SDL.h>
#endif

namespace pbr {

namespace {

bool ReadViaIfstream(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  out = buffer.str();
  return true;
}

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
bool ReadViaSdl(const std::string& path, std::string& out) {
  SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "rb");
  if (!stream) {
    return false;
  }
  const Sint64 length = SDL_GetIOSize(stream);
  if (length < 0) {
    SDL_CloseIO(stream);
    return false;
  }
  out.resize(static_cast<size_t>(length));
  const size_t read = SDL_ReadIO(stream, out.data(), static_cast<size_t>(length));
  SDL_CloseIO(stream);
  return read == static_cast<size_t>(length);
}
#endif

} // namespace

bool AssetIO::ReadText(const std::string& path, std::string& out) {
  if (path.empty()) {
    return false;
  }
  if (Platform::UsesPackagedAssets()) {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    return ReadViaSdl(path, out);
#else
    return false;
#endif
  }
  return ReadViaIfstream(path, out);
}

bool AssetIO::Exists(const std::string& path) {
  std::string ignored;
  return ReadText(path, ignored);
}

} // namespace pbr
