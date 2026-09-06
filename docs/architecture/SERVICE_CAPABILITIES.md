# Service capabilities & early UI

**Tier:** architecture  
**Related:** [UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md), [RUNTIME_COMPOSITION.md](RUNTIME_COMPOSITION.md), [WINDOW_SHELL.md](../ui/WINDOW_SHELL.md), [AT_REST_ENCRYPTION.md](../contracts/AT_REST_ENCRYPTION.md)

How functional **services** (behind ports) expose coarse readiness so the shell can paint early without waiting on mesh/network bring-up.

---

## Goal

Time-to-interactive UI should only wait for **local** ingredients:

1. Window + RmlUi shell mounted  
2. Vault unlocked (or PIN gate showing)  
3. Local messaging stack (identity + thread store + relay-capable orchestrator)

**Amp mesh**, reachability probes, and DHT are **not** on that path. They finish asynchronously and flip separate capability bits.

---

## Ports + services

| Layer | Owns |
|-------|------|
| **Service** (`ConversationsHub`, vault, …) | Lifecycle, retries, internal state, capability flags, action results |
| **Port / facade** | Snapshot/subscribe of flags + imperative actions |
| **Presenter** | Bind flags → enable/disable chrome; one generic “not ready” toast on coded failures |
| **Shell** | Paint as soon as local UI models exist; startup cover follows unlock-in-progress only |

Services answer from their own state. UI does **not** orchestrate bring-up beyond “ensure unlocked / ensure messaging ready.”

---

## Capability flags (messaging)

| Flag | Meaning | Enough for |
|------|---------|------------|
| `messaging_ready` | Vault open, identity loaded, local/relay messaging stack built | Browse threads, compose, relay send, contacts refresh |
| `mesh_ready` | Amp underlay started and attached (async after `messaging_ready`) | Call actions, direct P2P dial chrome |

Snapshots: `MessagingView` in `MessagingUiPorts.h` (`messaging_ready`, `mesh_ready`).  
Hub: `IsMessagingReady()`, `IsMeshReady()`; events `SetOnMessagingReady` / `SetOnMeshReady`.

**Rule:** Gate **entry points** (Send / Call buttons) on these flags. Do not push a full state machine into every presenter call site. If a deep link still fires early, the service returns a stable coded failure (`not_ready:mesh`, …) and UI shows one generic “Still connecting…” path.

---

## Startup sequence

```
first_present (shell + startup cover)
  → deferred unlock (Argon2 on worker)
  → EnsureMessagingReady
       → BuildLocalMessagingStack   // no StartMesh
       → messaging_ready = true     // cover may clear; compose enables
       → NotifyMessagingReady
       → ScheduleMeshBringUp (worker)
            → StartMesh (+ Node reachability probe may take seconds)
            → PostUI AttachAmpMessagingStack
            → mesh_ready = true/false
            → NotifyMeshReady       // call buttons enable when true
```

`unlock_in_progress` / startup cover must **not** wait on `StartMesh`. Measured cold start previously spent ~8s in a blocking Node dial-back probe inside `EnsureMessagingReady`; that work is now async.

---

## UI policy (keep it simple)

- Prefer **disable** over branching: Call hidden/disabled until `mesh_ready`; composer uses `messaging_ready`.
- One policy table in the presenter (`action → required capability`), not per-RPC handling.
- Services must be safe when called early (no crash, no UI-thread mesh start).

---

## Anti-patterns

- Blocking `EnsureMessagingReady` on network timeouts before flipping `messaging_ready`
- A single “ready” bit that conflates local store and mesh
- Presenters that special-case Argon2 vs DHT vs dial-back errors
- Functional code writing shell cover / chrome state except through unlock ports
