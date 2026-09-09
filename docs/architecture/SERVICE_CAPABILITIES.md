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

**Amp mesh**, reachability probes, DHT, cloud LLM keys, and client-compat checks are **not** on that path. They finish asynchronously and flip separate capability bits (or stay advisory).

---

## Ports + services

| Layer | Owns |
|-------|------|
| **Service** (`ConversationsHub`, vault, agent, …) | Lifecycle, retries, internal state, capability flags, action results |
| **Port / facade** | Snapshot/subscribe of flags + imperative actions |
| **Presenter** | Bind flags → enable/disable chrome; one generic “not ready” toast on coded failures |
| **Shell** | Paint as soon as local UI models exist; startup cover follows unlock-in-progress only |

Services answer from their own state. UI does **not** orchestrate bring-up beyond “ensure unlocked / ensure messaging ready.”

---

## Capability flags

| Flag | Owner / snapshot | Meaning | Enough for |
|------|------------------|---------|------------|
| `messaging_ready` | `MessagingView` / hub | Vault open, identity loaded, local/relay stack built | Browse threads, compose, relay send, contacts Message, Brief guest-key mint |
| `mesh_ready` | `MessagingView` / hub | Amp underlay started and attached (async after messaging) | Direct P2P dial chrome, Node hosting chrome |
| `call_ready` | **alias of `mesh_ready`** (MVP) | Mesh attached; call sessions already built at messaging-ready | Call / Accept entry buttons |
| `agent_cloud_ready` | `AgentView.cloud_ready` + `ChatController::AgentCloudReady()` | Agent configured and a usable LLM key exists (or mock) | AI / Brief turns — **not** peer relay send |
| `fonts_ready` | `ShellState` | Chrome labels safe (CJK deferred load done, or not needed) | Nav / home real strings vs placeholders |

**Not coarse flags (by design):**

| Signal | Where | Role |
|--------|-------|------|
| Reachability snapshot | Me → Network / statusbar | Node inbound posture (`node_reachable` detail); hosting chrome still needs `mesh_ready` |
| Client compat | `ClientCompatController::CheckAsync` | Advisory update / force-upgrade dialog — fail-open; never gates compose/call/nav |

Snapshots: `MessagingView` (`messaging_ready`, `mesh_ready`, `call_ready`), `AgentView` (`configured`, `cloud_ready`), shell `fonts_ready`.  
Hub: `IsMessagingReady()`, `IsMeshReady()`; events `SetOnMessagingReady` / `SetOnMeshReady`.

**Rule:** Gate **entry points** on these flags. Do not push a full state machine into every presenter call site. If a deep link still fires early, the service returns a stable coded failure (`not_ready:mesh`, `not_ready:agent_cloud`, …) and UI shows one generic “Still connecting…” / setup banner path.

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
            → mesh_ready / call_ready = true/false
            → NotifyMeshReady       // call + Node connection card enable when true
  → deferred fonts (UI); fonts_ready when CJK/emoji loaded (or skipped)
  → client-compat CheckAsync (advisory only)
```

`unlock_in_progress` / startup cover must **not** wait on `StartMesh`. Measured cold start previously spent ~8s in a blocking Node dial-back probe inside `EnsureMessagingReady`; that work is now async.

---

## Action → capability (presenter policy)

| User action | Required |
|-------------|----------|
| Open shell / browse local threads | shell + (after unlock) `messaging_ready` |
| Compose / relay send / contacts Message | `messaging_ready` |
| Call / Accept | `call_ready` (= `mesh_ready`) |
| Me → Network connection card / Node hosting toggles | `mesh_ready` (+ node enabled); details from reachability snapshot |
| AI / Brief cloud turn | `agent_cloud_ready` |
| Nav labels (CJK locales) | `fonts_ready` |

---

## UI policy (keep it simple)

- Prefer **disable** over branching: Call hidden until `call_ready`; composer uses `messaging_ready`; AI path checks `agent_cloud_ready` and falls back to the setup banner.
- One policy table in the presenter (`action → required capability`), not per-RPC handling.
- Services must be safe when called early (no crash, no UI-thread mesh start).
- Contacts **Message** stays on `messaging_ready` (local reachability helpers are not Amp dial). Call-from-contact would need `mesh_ready` separately if added later.

---

## Anti-patterns

- Blocking `EnsureMessagingReady` on network timeouts before flipping `messaging_ready`
- A single “ready” bit that conflates local store, mesh, LLM, and compat
- Presenters that special-case Argon2 vs DHT vs dial-back errors
- Gating the whole shell on client-compat or font load
- Functional code writing shell cover / chrome state except through unlock / setup ports

---

## Future splits (not MVP)

- **Calls:** `call_signaling_ready` vs `call_media_ready` only if media init leaves the mesh-attach path.
- **Node:** explicit `node_hosting_ready` vs snapshot-only reachability if Settings needs a bool without loading the full reachability view.
- **Agent:** `SetOnCloudReady` if configure/mint becomes fully async with a subscribe surface.
