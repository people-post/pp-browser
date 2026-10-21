#include "foundation/platform/IosBackupExclusion.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE

#import <Foundation/Foundation.h>

#include "common/Logger.h"
#include "common/PbrCompat.h"

namespace pbr {

bool ExcludeDirFromIosBackup(const std::string& dir) {
  @autoreleasepool {
    NSString* path = [NSString stringWithUTF8String:dir.c_str()];
    if (path == nil) {
      return false;
    }
    NSURL* url = [NSURL fileURLWithPath:path isDirectory:YES];
    NSError* error = nil;
    const bool set = [url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&error] == YES;
    // Read the attribute back from the system so the log shows what iOS actually recorded (PB-040).
    NSNumber* excluded = nil;
    NSError* read_error = nil;
    const bool read = [url getResourceValue:&excluded forKey:NSURLIsExcludedFromBackupKey error:&read_error] == YES;
    const bool readback = read && excluded != nil && [excluded boolValue];
    if (set && readback) {
      logging::getLogger("IosPaths").info << "data dir excluded from iCloud/iTunes backup (set=1 readback=1)";
    } else {
      logging::getLogger("IosPaths").warning
          << "data dir backup exclusion: set=" << set << " readback=" << readback
          << (error ? std::string(" error=") + [[error localizedDescription] UTF8String] : std::string());
    }
    return set;
  }
}

} // namespace pbr

#endif
#endif
