#pragma once

#include "mesh/host/MeshHost.h"
#include "common/Error.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * The relay services pp-node-relay runs on its node's mesh runtime (projects/node-relay NR001): the default pp-node
 * never links them. None yet — NR3 adds the mailbox (`/pp/rpc/relay/1.0.0`).
 *
 * A service that serves protocols must not call MeshHost::SetAppProtocols on its own: it replaces the whole list, and
 * the ledger role advertises through it too. NR3 combines them.
 *
 * Lifecycle: Start once the mesh is up; Stop before MeshHost::Stop.
 */
class RelayServices {
public:
  struct Options {
    /** The services' own directory (their store and settings); created by the services that need it. */
    std::string work_dir;
  };

  RelayServices() = default;
  ~RelayServices(); // Stop
  RelayServices(const RelayServices&) = delete;
  RelayServices& operator=(const RelayServices&) = delete;

  /** Starts the services on `mesh`'s runtime. The mesh must be started and outlive Stop. */
  Roe<void> Start(mesh::MeshHost& mesh, Options options);
  /** Stops every service. Idempotent. */
  void Stop();

  bool Started() const { return started_; }
  /** The services running, by name (for the log and /status); empty until NR3. */
  std::vector<std::string> Running() const;
  /** Why a service ended on its own; none while they run (or when none started). */
  std::optional<std::string> Failure() const;

private:
  bool started_ = false;
  Options options_;
};

} // namespace pbr
