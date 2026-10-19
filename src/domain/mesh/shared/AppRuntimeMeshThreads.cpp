// pp-browser's MeshThreads (pp-browser and pp-node): AppRuntime, the mesh on the Connectivity owner.
// The app's side of the seam — it stays in pp-browser when the mesh moves to pp-cpp-mesh.

#include "domain/mesh/shared/MeshThreads.h"

#include "foundation/runtime/AppRuntime.h"

namespace pbr {

namespace {
constexpr OwnerThreadId kOwner = OwnerThreadId::Connectivity;
} // namespace

bool MeshThreads::HasOwner() { return AppRuntime::HasOwner(kOwner); }

void MeshThreads::PostToOwner(Task task) { AppRuntime::PostTo(kOwner, std::move(task)); }

void MeshThreads::PostToOwnerOrRun(Task task) { AppRuntime::PostToOwnerOrRun(kOwner, std::move(task)); }

void MeshThreads::RunOnOwnerAndWait(const Task& task) { AppRuntime::RunAndWait(kOwner, task); }

uint64_t MeshThreads::ScheduleOnOwner(const std::chrono::milliseconds delay, Task task) {
  return AppRuntime::ScheduleOn(kOwner, delay, std::move(task));
}

void MeshThreads::AssertOnOwner(const char* where) { AppRuntime::AssertOn(kOwner, where); }

bool MeshThreads::OwnerTasksManual() { return AppRuntime::OwnerThreadsManual(); }

size_t MeshThreads::RunOwnerTasks() { return AppRuntime::RunOwnerTasks(kOwner); }

void MeshThreads::PostCoordinator(Task task) { AppRuntime::PostCoordinatorNormal(std::move(task)); }

uint64_t MeshThreads::ScheduleOneShot(const std::chrono::milliseconds delay, Task task) {
  return AppRuntime::ScheduleCoordinatorOneShot(delay, std::move(task));
}

void MeshThreads::CancelTimer(const uint64_t timer_id) { AppRuntime::CancelCoordinatorTimer(timer_id); }

bool MeshThreads::IsRunning() { return AppRuntime::IsRunning(); }

void MeshThreads::PostWorker(const pp::WorkerLane lane, Task task) { AppRuntime::PostWorker(lane, std::move(task)); }

void MeshThreads::PostToApp(Task task) { AppRuntime::PostUI(std::move(task)); }

bool MeshThreads::IsShuttingDown() { return AppRuntime::IsShuttingDown(); }

} // namespace pbr
