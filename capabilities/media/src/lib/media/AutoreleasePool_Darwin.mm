#include "lib/media/AutoreleasePool.h"

// The runtime's pool entry points (what @autoreleasepool compiles to); a scope object cannot hold
// an @autoreleasepool block across its constructor and destructor.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* token);

namespace pbr {

AutoreleasePoolScope::AutoreleasePoolScope() : token_(objc_autoreleasePoolPush()) {}

AutoreleasePoolScope::~AutoreleasePoolScope() {
  objc_autoreleasePoolPop(token_);
}

} // namespace pbr
