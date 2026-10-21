#pragma once

#include "feature/conversations/HubPolicyPrefs.h"
#include "foundation/platform/NetworkMonitor.h"
#include "mesh/host/LocalNetworkChange.h"
#include "common/Error.h"

#include <functional>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

class ConversationsHub;
class IDekConsumer;

/**
 * A feature that rides the hub's messaging and mesh: calls, live broadcast. The hub drives it at
 * the points of its own lifecycle without knowing what it is. Feature-level wiring
 * (feature/product) adapts each feature to this and registers it before the hub initializes.
 *
 * Hooks run on the hub's (UI) thread unless noted, in registration order (reverse order on the
 * way down).
 */
class HubParticipant {
public:
  virtual ~HubParticipant() = default;

  /** The profile's stores are open (no messaging, no mesh yet). An error fails initialization. */
  virtual Roe<void> OnStoresOpened(ConversationsHub& hub) { return {}; }
  /** Vault-backed state the participant owns: registered and unregistered with the hub's own. */
  virtual std::vector<IDekConsumer*> DekConsumers() { return {}; }
  /** A messaging stack was built (once at initialization, again at unlock). */
  virtual void OnMessagingBuilt(ConversationsHub& hub) {}
  /** Policy prefs, after a messaging stack is built and on every change. */
  virtual void OnPolicyPrefs(const HubPolicyPrefs& prefs) {}
  /** Teardown is starting: drop callbacks into UI-bound objects. */
  virtual void OnTeardownStarting() {}
  /** The messaging stack is going away (the mesh has stopped): drop what holds it. */
  virtual void OnMessagingDropped() {}
  /** Final teardown: free everything (DEKs already unregistered). */
  virtual void OnShutdown() {}

  /** The mesh's media objects (connectivity, media relay) are about to be rewired: let go of them. */
  virtual void OnMeshMediaDetach() {}
  /** Mesh services are up for the first time on this mesh (media objects wired). */
  virtual void OnMeshServicesStarted(ConversationsHub& hub) {}
  /** The media objects were rewired after a detach (capability refresh). */
  virtual void OnMeshMediaRebound(ConversationsHub& hub) {}
  /** The mesh is stopping: let go of it. `abort_circuits` unblocks in-flight circuit waits. */
  virtual void OnMeshStopping(const std::function<void()>& abort_circuits) {}
  /** The mesh host stopped (its workers joined): free what used it. */
  virtual void OnMeshStopped() {}

  /** Our public mapping moved (a NAT rebind, a network change's re-probe). */
  virtual void OnObservedAddressChanged() {}
  /**
   * The device's network changed. Any thread. `local` is what the mesh made of it; null for the
   * baseline report (generation 0), which only classifies.
   */
  virtual void OnLocalNetworkChanged(const NetworkChange& change, const mesh::LocalNetworkChange* local) {}
  /** Shutdown was requested: unblock waits. Any thread. */
  virtual void OnShutdownRequested() {}

  /** Mobile: whether an ephemeral listen should stay up for this participant (an active call). */
  virtual bool WantsEphemeralListen() const { return false; }
};

} // namespace pbr
