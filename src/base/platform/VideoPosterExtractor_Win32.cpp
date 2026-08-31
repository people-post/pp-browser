#include "base/platform/VideoPosterExtractor.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

using Microsoft::WRL::ComPtr;

Roe<std::vector<uint8_t>> EncodeJpegBytes(SDL_Surface* surface, const int quality) {
  SDL_IOStream* io = SDL_IOFromDynamicMem();
  if (!io) {
    return Error("Failed to allocate JPEG buffer");
  }
  if (!IMG_SaveJPG_IO(surface, io, false, quality)) {
    SDL_CloseIO(io);
    return Error("Failed to encode video poster JPEG");
  }
  const Sint64 size = SDL_GetIOSize(io);
  if (size <= 0) {
    SDL_CloseIO(io);
    return Error("Encoded video poster JPEG is empty");
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (SDL_SeekIO(io, 0, SDL_IO_SEEK_SET) < 0) {
    SDL_CloseIO(io);
    return Error("Failed to rewind video poster JPEG buffer");
  }
  if (SDL_ReadIO(io, bytes.data(), bytes.size()) != static_cast<size_t>(size)) {
    SDL_CloseIO(io);
    return Error("Failed to read video poster JPEG buffer");
  }
  SDL_CloseIO(io);
  return bytes;
}

SDL_Surface* ScaleToMaxDimension(SDL_Surface* source, const int max_dimension) {
  if (!source) {
    return nullptr;
  }
  const int src_w = source->w;
  const int src_h = source->h;
  if (src_w <= 0 || src_h <= 0) {
    return nullptr;
  }
  const int max_side = std::max(src_w, src_h);
  if (max_side <= max_dimension) {
    return SDL_DuplicateSurface(source);
  }
  const double scale = static_cast<double>(max_dimension) / static_cast<double>(max_side);
  const int dst_w = std::max(1, static_cast<int>(std::lround(src_w * scale)));
  const int dst_h = std::max(1, static_cast<int>(std::lround(src_h * scale)));
  return SDL_ScaleSurface(source, dst_w, dst_h, SDL_SCALEMODE_LINEAR);
}

std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring wide(static_cast<size_t>(needed - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), needed);
  return wide;
}

Roe<std::vector<uint8_t>> ExtractWithMediaFoundation(const std::string& video_path, const int max_dimension) {
  if (video_path.empty()) {
    return Error("Video path empty");
  }
  const std::wstring wide = Utf8ToWide(video_path);
  if (wide.empty()) {
    return Error("Invalid video path encoding");
  }

  const HRESULT init_hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  const bool started = SUCCEEDED(init_hr);
  if (!started && init_hr != MF_E_SHUTDOWN) {
    // Continue if already started elsewhere; otherwise fail soft.
    if (FAILED(init_hr)) {
      return Error("MFStartup failed");
    }
  }

  ComPtr<IMFSourceReader> reader;
  HRESULT hr = MFCreateSourceReaderFromURL(wide.c_str(), nullptr, &reader);
  if (FAILED(hr) || !reader) {
    if (started) {
      MFShutdown();
    }
    return Error("MFCreateSourceReaderFromURL failed");
  }

  ComPtr<IMFMediaType> partial;
  hr = MFCreateMediaType(&partial);
  if (FAILED(hr)) {
    if (started) {
      MFShutdown();
    }
    return Error("MFCreateMediaType failed");
  }
  partial->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  partial->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, partial.Get());
  if (FAILED(hr)) {
    if (started) {
      MFShutdown();
    }
    return Error("SetCurrentMediaType RGB32 failed");
  }

  DWORD stream_flags = 0;
  LONGLONG timestamp = 0;
  ComPtr<IMFSample> sample;
  hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &stream_flags,
                          &timestamp, &sample);
  if (FAILED(hr) || !sample) {
    if (started) {
      MFShutdown();
    }
    return Error("ReadSample failed");
  }

  ComPtr<IMFMediaType> current;
  hr = reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current);
  UINT32 width = 0;
  UINT32 height = 0;
  if (FAILED(hr) || FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width, &height)) || width == 0 ||
      height == 0) {
    if (started) {
      MFShutdown();
    }
    return Error("Failed to read frame size");
  }

  ComPtr<IMFMediaBuffer> buffer;
  hr = sample->ConvertToContiguousBuffer(&buffer);
  if (FAILED(hr) || !buffer) {
    if (started) {
      MFShutdown();
    }
    return Error("ConvertToContiguousBuffer failed");
  }

  BYTE* data = nullptr;
  DWORD max_len = 0;
  DWORD cur_len = 0;
  hr = buffer->Lock(&data, &max_len, &cur_len);
  if (FAILED(hr) || !data || cur_len == 0) {
    if (started) {
      MFShutdown();
    }
    return Error("IMFMediaBuffer Lock failed");
  }

  SDL_Surface* surface =
      SDL_CreateSurface(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_XRGB8888);
  if (!surface) {
    buffer->Unlock();
    if (started) {
      MFShutdown();
    }
    return Error("Failed to create SDL surface");
  }

  // MF RGB32 is typically bottom-up BGRX; copy row-by-row flipping vertically.
  const size_t src_stride = static_cast<size_t>(width) * 4u;
  for (UINT32 y = 0; y < height; ++y) {
    const UINT32 src_y = height - 1u - y;
    if (src_y * src_stride + src_stride > cur_len) {
      break;
    }
    uint8_t* dst = static_cast<uint8_t*>(surface->pixels) + static_cast<size_t>(y) * static_cast<size_t>(surface->pitch);
    const BYTE* src = data + src_y * src_stride;
    std::memcpy(dst, src, src_stride);
  }
  buffer->Unlock();

  SDL_Surface* scaled = ScaleToMaxDimension(surface, std::max(16, max_dimension));
  SDL_DestroySurface(surface);
  if (!scaled) {
    if (started) {
      MFShutdown();
    }
    return Error("Failed to scale video poster");
  }
  auto jpeg = EncodeJpegBytes(scaled, 80);
  SDL_DestroySurface(scaled);
  if (started) {
    MFShutdown();
  }
  return jpeg;
}

} // namespace

Roe<std::vector<uint8_t>> ExtractVideoPosterJpeg(const std::string& video_path, const int max_dimension) {
  if (auto jpeg = ExtractWithMediaFoundation(video_path, max_dimension)) {
    return jpeg;
  }
  return SoftVideoPosterJpeg(max_dimension);
}

} // namespace pbr

#else

namespace pbr {

Roe<std::vector<uint8_t>> ExtractVideoPosterJpeg(const std::string& video_path, const int max_dimension) {
  (void)video_path;
  return SoftVideoPosterJpeg(max_dimension);
}

} // namespace pbr

#endif
