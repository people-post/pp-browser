#include "domain/node/AppRuntimeMeshExecutors.h"

#include "foundation/runtime/AppRuntime.h"
#include "common/MeshAlias.h"

namespace pbr {

mesh::MeshExecutors AppRuntimeMeshExecutors() {
  mesh::MeshExecutors executors;
  executors.work = AppRuntime::WorkerExecutor(WorkerLane::Normal);
  executors.blocking = AppRuntime::WorkerExecutor(WorkerLane::Background);
  return executors;
}

} // namespace pbr
