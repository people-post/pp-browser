#include "base/platform/VideoPosterExtractor.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

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

Roe<std::vector<uint8_t>> ExtractWithAvFoundation(const std::string& video_path, const int max_dimension) {
  @autoreleasepool {
    if (video_path.empty()) {
      return Error("Video path empty");
    }
    NSString* path = [NSString stringWithUTF8String:video_path.c_str()];
    if (!path || ![[NSFileManager defaultManager] fileExistsAtPath:path]) {
      return Error("Video file missing");
    }
    NSURL* url = [NSURL fileURLWithPath:path];
    AVAsset* asset = [AVAsset assetWithURL:url];
    if (!asset) {
      return Error("Failed to open AVAsset");
    }
    AVAssetImageGenerator* gen = [[AVAssetImageGenerator alloc] initWithAsset:asset];
    gen.appliesPreferredTrackTransform = YES;
    gen.maximumSize = CGSizeMake(static_cast<CGFloat>(std::max(16, max_dimension)),
                                 static_cast<CGFloat>(std::max(16, max_dimension)));

    const CMTime at = CMTimeMake(1, 10); // ~0.1s
    NSError* err = nil;
    CGImageRef image = [gen copyCGImageAtTime:at actualTime:nullptr error:&err];
    if (!image) {
      image = [gen copyCGImageAtTime:kCMTimeZero actualTime:nullptr error:&err];
    }
    if (!image) {
      return Error("AVAssetImageGenerator failed");
    }

    const size_t width = CGImageGetWidth(image);
    const size_t height = CGImageGetHeight(image);
    if (width == 0 || height == 0) {
      CGImageRelease(image);
      return Error("Empty CGImage from video");
    }

    SDL_Surface* surface =
        SDL_CreateSurface(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
      CGImageRelease(image);
      return Error("Failed to create SDL surface for poster");
    }

    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    // Cast: OR of CGImageAlphaInfo and anonymous byte-order enum is deprecated on newer SDKs.
    const CGBitmapInfo bitmap_info =
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big;
    CGContextRef ctx = CGBitmapContextCreate(surface->pixels, width, height, 8, static_cast<size_t>(surface->pitch),
                                             space, bitmap_info);
    CGColorSpaceRelease(space);
    if (!ctx) {
      SDL_DestroySurface(surface);
      CGImageRelease(image);
      return Error("Failed to create CGBitmapContext");
    }
    CGContextDrawImage(ctx, CGRectMake(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height)), image);
    CGContextRelease(ctx);
    CGImageRelease(image);

    SDL_Surface* scaled = ScaleToMaxDimension(surface, std::max(16, max_dimension));
    SDL_DestroySurface(surface);
    if (!scaled) {
      return Error("Failed to scale video poster");
    }
    auto jpeg = EncodeJpegBytes(scaled, 80);
    SDL_DestroySurface(scaled);
    return jpeg;
  }
}

} // namespace

Roe<std::vector<uint8_t>> ExtractVideoPosterJpeg(const std::string& video_path, const int max_dimension) {
  if (auto jpeg = ExtractWithAvFoundation(video_path, max_dimension)) {
    return jpeg;
  }
  return SoftVideoPosterJpeg(max_dimension);
}

} // namespace pbr
