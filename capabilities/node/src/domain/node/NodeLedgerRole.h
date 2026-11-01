#pragma once

#include "mesh/host/MeshHost.h"
#include "common/Error.h"

#include <memory>
#include <optional>
#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * pp-node's ledger role (pp-ledger platform-integration pi4): one pp-ledger relay or miner, run on
 * this node's mesh runtime with the node's identity and address, from a work directory of its own
 * (pp-ledger's `config.json`, keys and data — PI003, PI004). The role's thread is its own; it
 * advertises the ledger's protocols on the mesh while their handlers are bound, and withdraws them
 * before they go.
 *
 * Lifecycle: Start once the mesh is up; Stop (stop → join) before MeshHost::Stop.
 */
class NodeLedgerRole {
public:
  enum class Role { Relay, Miner };
  /** "relay" / "miner"; anything else (a beacon is standalone only) is none. */
  static std::optional<Role> RoleNamed(const std::string& name);

  struct Options {
    Role role = Role::Relay;
    /** pp-ledger's work directory (created when missing; its `config.json` is the operator's). */
    std::string work_dir;
  };

  NodeLedgerRole();
  ~NodeLedgerRole();  // Stop
  NodeLedgerRole(const NodeLedgerRole&) = delete;
  NodeLedgerRole& operator=(const NodeLedgerRole&) = delete;

  /** Runs the role on `mesh`'s runtime. The mesh must be started and outlive Stop. */
  Roe<void> Start(mesh::MeshHost& mesh, Options options);
  /** Stops the role and joins its thread; the ledger's protocols are withdrawn by then. Idempotent. */
  void Stop();

  /** The ledger's protocol handlers are bound (and advertised). */
  bool Listening() const;
  /** Why the role ended on its own (it does not until stopped, unless it fails); none while it runs. */
  std::optional<std::string> Failure() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace pbr
