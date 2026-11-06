// macOS and iOS: a sound file's title, artist, length and cover through AVAsset's common metadata (ID3 TIT2 / TPE1 /
// APIC in an MP3, the iTunes atoms of an M4A). Objective-C++ without ARC. Called off the main thread.
#include "lib/voice/AudioFileTags.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>

namespace pbr {

std::optional<AudioFileTags> ReadAudioFileTags(const std::string& path) {
  @autoreleasepool {
    NSString* file = [NSString stringWithUTF8String:path.c_str()];
    if (file == nil) {
      return std::nullopt;
    }
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:file] options:nil];
    if (asset == nil) {
      return std::nullopt;
    }
    AudioFileTags tags;
    for (AVMetadataItem* item in asset.commonMetadata) {
      NSString* key = item.commonKey;
      if ([key isEqualToString:AVMetadataCommonKeyTitle] && tags.title.empty() && item.stringValue.length > 0) {
        tags.title = item.stringValue.UTF8String;
      } else if ([key isEqualToString:AVMetadataCommonKeyArtist] && tags.artist.empty() &&
                 item.stringValue.length > 0) {
        tags.artist = item.stringValue.UTF8String;
      } else if ([key isEqualToString:AVMetadataCommonKeyArtwork] && tags.picture.empty()) {
        NSData* data = item.dataValue;
        if (data.length > 0 && data.length <= kMaxAudioCoverBytes) {
          const auto* bytes = static_cast<const uint8_t*>(data.bytes);
          tags.picture.assign(bytes, bytes + data.length);
        }
      }
    }
    const CMTime duration = asset.duration;
    if (CMTIME_IS_NUMERIC(duration)) {
      tags.duration_ms = static_cast<int64_t>(CMTimeGetSeconds(duration) * 1000.0);
    }
    return tags;
  }
}

} // namespace pbr
