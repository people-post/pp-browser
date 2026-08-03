#pragma once

#include "common/WorkerDispatch.h"
#include "common/WorkerPool.h"
#include "libp2p/integration/host/Libp2pHost.h"

#include <functional>

namespace pbr {

/** Prefer AppRuntime::PostWorker in app/feature code; use this from libp2p integration. */
inline void PostLibp2pWorker(Libp2pHost& host, WorkerLane lane, std::function<void()> task) {
  if (WorkerDispatch::IsInstalled()) {
    WorkerDispatch::Post(lane, std::move(task));
  } else {
    host.GetWorkerPool().Post(lane, std::move(task));
  }
}

} // namespace pbr
