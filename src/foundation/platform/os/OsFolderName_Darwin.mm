#include "foundation/platform/os/OsFolderName.h"

#include "common/PathUtf8.h"

#import <Foundation/Foundation.h>

namespace pbr::os {

std::string FolderDisplayName(const std::string& path) {
  @autoreleasepool {
    NSString* folder = path.empty() ? nil : [NSString stringWithUTF8String:path.c_str()];
    NSString* shown = folder != nil ? [[NSFileManager defaultManager] displayNameAtPath:folder] : nil;
    if (shown.length > 0) {
      return std::string(shown.UTF8String);
    }
  }
  const std::filesystem::path name = PathFromUtf8(path).filename();
  return name.empty() ? path : PathToUtf8(name);
}

} // namespace pbr::os
