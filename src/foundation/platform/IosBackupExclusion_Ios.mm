#include "foundation/platform/IosBackupExclusion.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE

#import <Foundation/Foundation.h>

namespace pbr {

bool ExcludeDirFromIosBackup(const std::string& dir) {
  @autoreleasepool {
    NSString* path = [NSString stringWithUTF8String:dir.c_str()];
    if (path == nil) {
      return false;
    }
    NSURL* url = [NSURL fileURLWithPath:path isDirectory:YES];
    NSError* error = nil;
    return [url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&error] == YES;
  }
}

} // namespace pbr

#endif
#endif
