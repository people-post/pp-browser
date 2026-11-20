#include "lib/media/AutoreleasePool.h"

#import <Foundation/Foundation.h>

#include <gtest/gtest.h>

static bool g_deallocated = false;

@interface PbrPoolProbe : NSObject
@end

@implementation PbrPoolProbe
- (void)dealloc {
  g_deallocated = true;
  [super dealloc];
}
@end

namespace pbr {
namespace {

// A plain thread has no pool: an object autoreleased inside the scope must die when the scope ends.
TEST(AutoreleasePoolScopeDarwinTest, AutoreleasedObjectDiesWithTheScope) {
  g_deallocated = false;
  {
    AutoreleasePoolScope pool;
    [[[PbrPoolProbe alloc] init] autorelease];
    EXPECT_FALSE(g_deallocated);
  }
  EXPECT_TRUE(g_deallocated);
}

} // namespace
} // namespace pbr
