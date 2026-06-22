#include "platform/AndroidFileInterface.h"

#if defined(__ANDROID__)

#include <SDL3/SDL.h>

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

Rml::FileHandle AndroidFileInterface::Open(const Rml::String& path) {
  SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "rb");
  return reinterpret_cast<Rml::FileHandle>(stream);
}

void AndroidFileInterface::Close(Rml::FileHandle file) {
  SDL_CloseIO(reinterpret_cast<SDL_IOStream*>(file));
}

size_t AndroidFileInterface::Read(void* buffer, size_t size, Rml::FileHandle file) {
  return SDL_ReadIO(reinterpret_cast<SDL_IOStream*>(file), buffer, size);
}

bool AndroidFileInterface::Seek(Rml::FileHandle file, long offset, int origin) {
  return SDL_SeekIO(reinterpret_cast<SDL_IOStream*>(file), static_cast<Sint64>(offset), SeekOrigin(origin)) >= 0;
}

size_t AndroidFileInterface::Tell(Rml::FileHandle file) {
  const Sint64 position = SDL_TellIO(reinterpret_cast<SDL_IOStream*>(file));
  return position >= 0 ? static_cast<size_t>(position) : 0;
}

} // namespace pbr

#endif
