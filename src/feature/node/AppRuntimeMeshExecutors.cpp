#include "feature/node/AppRuntimeMeshExecutors.h"

#include "foundation/runtime/AppRuntime.h"

namespace pbr {

MeshExecutors AppRuntimeMeshExecutors() {
  MeshExecutors executors;
  executors.work = AppRuntime::WorkerExecutor(WorkerLane::Normal);
  executors.blocking = AppRuntime::WorkerExecutor(WorkerLane::Background);
  return executors;
}

} // namespace pbr
