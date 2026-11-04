#include "foundation/platform/ImageThumbnail.h"

#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

template <typename T>
struct CfRelease {
  T ref = nullptr;
  explicit CfRelease(T r) : ref(r) {}
  ~CfRelease() {
    if (ref) {
      CFRelease(ref);
    }
  }
  CfRelease(const CfRelease&) = delete;
  CfRelease& operator=(const CfRelease&) = delete;
};

} // namespace

Roe<std::vector<uint8_t>> MakeImageThumbnailJpeg(const std::vector<uint8_t>& image_bytes, const int max_dimension) {
  if (image_bytes.empty() || max_dimension <= 0) {
    return Error("Image bytes and size are required");
  }
  // A view over the caller's bytes (no copy); they outlive every use below.
  CfRelease<CFDataRef> data_in(CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, image_bytes.data(),
                                                            static_cast<CFIndex>(image_bytes.size()), kCFAllocatorNull));
  if (!data_in.ref) {
    return Error("Cannot read image");
  }
  CfRelease<CGImageSourceRef> source(CGImageSourceCreateWithData(data_in.ref, nullptr));
  if (!source.ref) {
    return Error("Cannot read image");
  }

  // Downsampled decode: the full-size pixels are never held.
  const int max_side = max_dimension;
  CfRelease<CFNumberRef> size(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &max_side));
  const void* keys[] = {kCGImageSourceCreateThumbnailFromImageAlways, kCGImageSourceCreateThumbnailWithTransform,
                        kCGImageSourceThumbnailMaxPixelSize, kCGImageSourceShouldCache};
  const void* values[] = {kCFBooleanTrue, kCFBooleanTrue, size.ref, kCFBooleanFalse};
  CfRelease<CFDictionaryRef> options(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 4,
                                                        &kCFTypeDictionaryKeyCallBacks,
                                                        &kCFTypeDictionaryValueCallBacks));
  CfRelease<CGImageRef> thumb(CGImageSourceCreateThumbnailAtIndex(source.ref, 0, options.ref));
  if (!thumb.ref) {
    return Error("Cannot decode image");
  }

  CfRelease<CFMutableDataRef> data(CFDataCreateMutable(kCFAllocatorDefault, 0));
  CfRelease<CGImageDestinationRef> dest(CGImageDestinationCreateWithData(data.ref, CFSTR("public.jpeg"), 1, nullptr));
  if (!dest.ref) {
    return Error("Cannot encode JPEG");
  }
  const float quality = 0.8f;
  CfRelease<CFNumberRef> quality_number(CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &quality));
  const void* dest_keys[] = {kCGImageDestinationLossyCompressionQuality};
  const void* dest_values[] = {quality_number.ref};
  CfRelease<CFDictionaryRef> dest_options(CFDictionaryCreate(kCFAllocatorDefault, dest_keys, dest_values, 1,
                                                             &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks));
  CGImageDestinationAddImage(dest.ref, thumb.ref, dest_options.ref);
  if (!CGImageDestinationFinalize(dest.ref)) {
    return Error("Cannot encode JPEG");
  }
  const CFIndex length = CFDataGetLength(data.ref);
  if (length <= 0) {
    return Error("Encoded thumbnail is empty");
  }
  const UInt8* bytes = CFDataGetBytePtr(data.ref);
  return std::vector<uint8_t>(bytes, bytes + length);
}

} // namespace pbr
