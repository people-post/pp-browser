#include "base/platform/SdlAssetFileInterface.h"

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)

#include <SDL3/SDL.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__ANDROID__) || TARGET_OS_IPHONE

namespace pbr {

namespace {

SDL_IOWhence SeekOrigin(int origin) {
  switch (origin) {
  case SEEK_SET:
    return SDL_IO_SEEK_SET;
  case SEEK_CUR:
    return SDL_IO_SEEK_CUR;
  case SEEK_END:
    return SDL_IO_SEEK_END;
  default:
    return SDL_IO_SEEK_SET;
  }
}

} // namespace

Rml::FileHandle SdlAssetFileInterface::Open(const Rml::String& path) {
  SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "rb");
  return reinterpret_cast<Rml::FileHandle>(stream);
}

void SdlAssetFileInterface::Close(Rml::FileHandle file) {
  SDL_CloseIO(reinterpret_cast<SDL_IOStream*>(file));
}

size_t SdlAssetFileInterface::Read(void* buffer, size_t size, Rml::FileHandle file) {
  return SDL_ReadIO(reinterpret_cast<SDL_IOStream*>(file), buffer, size);
}

bool SdlAssetFileInterface::Seek(Rml::FileHandle file, long offset, int origin) {
  return SDL_SeekIO(reinterpret_cast<SDL_IOStream*>(file), static_cast<Sint64>(offset), SeekOrigin(origin)) >= 0;
}

size_t SdlAssetFileInterface::Tell(Rml::FileHandle file) {
  const Sint64 position = SDL_TellIO(reinterpret_cast<SDL_IOStream*>(file));
  return position >= 0 ? static_cast<size_t>(position) : 0;
}

} // namespace pbr

#endif
#endif
