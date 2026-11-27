#pragma once

#include "mesh/host/MeshHost.h"
#include "common/Error.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

/**
 * Add-on services (projects/node-relay NR001): what pp-node-plus runs beside the node's own and the default pp-node
 * never links. Every add-on capability's code is in this namespace, so one symbol check keeps them all out of pp-node
 * (src/app/node/CMakeLists.txt).
 */
namespace pbr::plus {

/**
 * The message relay, pp-node-plus's first add-on: a relay of the relay protocol on the node's mesh runtime. Empty
 * until NR3 adds the mailbox (`/pp/rpc/relay/1.0.0`).
 *
 * It must not call MeshHost::SetAppProtocols on its own: that replaces the whole list, and the ledger role advertises
 * through it too. NR3 combines them.
 *
 * Lifecycle: Start once the mesh is up; Stop before MeshHost::Stop.
 */
class RelayService {
public:
  struct Options {
    /** The service's own directory (its store and settings). */
    std::string work_dir;
  };

  RelayService() = default;
  ~RelayService(); // Stop
  RelayService(const RelayService&) = delete;
  RelayService& operator=(const RelayService&) = delete;

  /** Starts the service on `mesh`'s runtime. The mesh must be started and outlive Stop. */
  Roe<void> Start(mesh::MeshHost& mesh, Options options);
  /** Stops the service. Idempotent. */
  void Stop();

  bool Started() const { return started_; }
  /** Why the service ended on its own; none while it runs (or when it did not start). */
  std::optional<std::string> Failure() const;

private:
  bool started_ = false;
  Options options_;
};

} // namespace pbr::plus
