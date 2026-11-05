#include "lib/media/MediaThreadName.h"

#include <pthread.h>

namespace pbr {

void SetMediaThreadName(const char* name) {
  pthread_setname_np(name);
}

} // namespace pbr
