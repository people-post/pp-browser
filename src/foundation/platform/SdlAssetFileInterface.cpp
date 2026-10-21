#include "foundation/platform/SdlAssetFileInterface.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)

#include <SDL3/SDL.h>

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

ui::FileHandle SdlAssetFileInterface::Open(const ui::String& path) {
  SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "rb");
  // RmlUi strips the leading '/' of an absolute path (SystemInterface::JoinPath) and the working directory
  // here is the app bundle, so a file in the data dir (an image thumbnail, an attachment view) arrives as
  // "private/var/..." and is not found: the <img> then drew a white box. Try the absolute path it was.
  if (stream == nullptr && !path.empty() && path[0] != '/') {
    const ui::String absolute = "/" + path;
    stream = SDL_IOFromFile(absolute.c_str(), "rb");
  }
  return reinterpret_cast<ui::FileHandle>(stream);
}

void SdlAssetFileInterface::Close(ui::FileHandle file) {
  SDL_CloseIO(reinterpret_cast<SDL_IOStream*>(file));
}

size_t SdlAssetFileInterface::Read(void* buffer, size_t size, ui::FileHandle file) {
  return SDL_ReadIO(reinterpret_cast<SDL_IOStream*>(file), buffer, size);
}

bool SdlAssetFileInterface::Seek(ui::FileHandle file, long offset, int origin) {
  return SDL_SeekIO(reinterpret_cast<SDL_IOStream*>(file), static_cast<Sint64>(offset), SeekOrigin(origin)) >= 0;
}

size_t SdlAssetFileInterface::Tell(ui::FileHandle file) {
  const Sint64 position = SDL_TellIO(reinterpret_cast<SDL_IOStream*>(file));
  return position >= 0 ? static_cast<size_t>(position) : 0;
}

} // namespace pbr

#endif
#endif
