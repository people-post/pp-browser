#pragma once

#include "common/thread/IThreadCatalog.h"
#include "common/thread/IThreadMemory.h"
#include "common/thread/IThreadSync.h"
#include "common/thread/IThreadTranscript.h"

namespace pbr {

/** Full thread store seam — prefers role ports (catalog/transcript/memory/sync) at call sites. */
class IThreadStore : public IThreadCatalog,
                     public IThreadTranscript,
                     public IThreadMemory,
                     public IThreadSync {
public:
  ~IThreadStore() override = default;
  virtual void Flush() = 0;
};

} // namespace pbr
