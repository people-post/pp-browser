#include "domain/mesh/host/MeshControlDispatch.h"

#include "domain/mesh/host/MeshControlPool.h"

#include <cassert>

namespace pbr {

namespace {

MeshControlPool* g_mesh_control = nullptr;

} // namespace

void MeshControlDispatch::Install(MeshControlPool* pool) {
  g_mesh_control = pool;
}

void MeshControlDispatch::Uninstall() {
  g_mesh_control = nullptr;
}

bool MeshControlDispatch::IsInstalled() {
  return g_mesh_control != nullptr;
}

void MeshControlDispatch::Post(std::function<void()> task) {
  if (!task) {
    return;
  }
  if (!g_mesh_control) {
    assert(false && "MeshControlDispatch not installed (MeshHost Amp not started)");
    return;
  }
  g_mesh_control->Post(std::move(task));
}

} // namespace pbr
