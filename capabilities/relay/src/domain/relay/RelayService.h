#pragma once

#include "mesh/host/MeshHost.h"
#include "common/Error.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

/**
 * The relay capability (projects/node-relay NR001). A node service's code is in its own namespace, pbr::<service>:
 * the symbol check that keeps a build's services out of the other build is made from the service's name
 * (src/app/node/CMakeLists.txt), so moving a service between pp-node and pp-node-plus renames nothing here.
 */
namespace pbr::relay {

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

} // namespace pbr::relay
