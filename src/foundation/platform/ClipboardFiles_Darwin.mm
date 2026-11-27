#include "foundation/platform/ClipboardFiles.h"

#import <AppKit/AppKit.h>

namespace pbr {
namespace {

// D2 kill switch for macOS: set to false to leave a Finder copy to the text field (D1 behaviour) on this platform only.
constexpr bool kFilePasteEnabled = true;

NSDictionary* FileUrlsOnly() {
  return @{NSPasteboardURLReadingFileURLsOnlyKey : @YES};
}

} // namespace

bool ClipboardImageQueryIsCheap() {
  return true;
}

bool ClipboardFilePasteEnabled() {
  return kFilePasteEnabled;
}

bool ClipboardHasFiles() {
  @autoreleasepool {
    return [NSPasteboard.generalPasteboard canReadObjectForClasses:@[ NSURL.class ] options:FileUrlsOnly()];
  }
}

std::vector<std::string> ReadClipboardFiles() {
  std::vector<std::string> files;
  @autoreleasepool {
    NSArray* urls = [NSPasteboard.generalPasteboard readObjectsForClasses:@[ NSURL.class ] options:FileUrlsOnly()];
    for (id item in urls) {
      if (![item isKindOfClass:NSURL.class] || ![(NSURL*)item isFileURL]) {
        continue;
      }
      NSString* path = [(NSURL*)item path];
      if (path.length > 0) {
        files.emplace_back(path.UTF8String);
      }
    }
  }
  return files;
}

} // namespace pbr
