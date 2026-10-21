# Ownership — parent-only destroy

**Tier:** architecture  
**Related:** [RUNTIME_COMPOSITION.md](RUNTIME_COMPOSITION.md) (who owns what at the composition root), [COMPOSITION_VOCABULARY.md](COMPOSITION_VOCABULARY.md) (no upward concepts through ports), [SRC_LAYOUT.md](SRC_LAYOUT.md) (layer edges), [THREADING.md](THREADING.md) (affinity), mesh specialization [A027](../../projects/adp/DECISIONS.md#a027--parent-only-destroy-l3l4-ownership-hierarchy) / [AMP-CHANNEL § Ownership](../contracts/AMP-CHANNEL.md#ownership-hierarchy-a027).

## Rule

**Only the parent may destroy a child.**  
Children and callbacks may **request** shutdown; they must not drop the durable owner (last owning `unique_ptr`/`shared_ptr`, map erase of the owning entry, or unbind of the parent’s registration) while still on that child’s stack.

This is a **repo-wide** convention (UI, media, stores, mesh, tests). Mesh L3/L4 detail lives in A027; everything else uses this page.

## Request vs destroy

| Request (child / callback OK) | Destroy (parent only) |
|-------------------------------|------------------------|
| `Close` / `CloseQuiet` / return `false` from a frame handler | `reset()` the owning slot / `erase` from the owning container |
| `on_closed` / “please stop” notify | Unbind handlers the parent registered (`ReleaseHandlers`, clear mux/`std::function` slots) |
| Post “teardown needed” to the owner’s thread | Free the object on the wrong affinity (see below) |

RAII in a child’s destructor freeing *that child’s* resources is fine. A child deleting or `reset()`-ing its parent (or erasing itself from the parent’s map mid-callback so the last ref dies before return) is not.

## Durable owner vs dispatch pin

- **Durable owner:** the clear slot that defines lifetime (composition `unique_ptr`, bundle field, `sessions[id]`, test fixture member).
- **Dispatch pin:** a short-lived `shared_ptr` / `shared_from_this` lock held only for the current callback so TearDown-from-signal does not UAF. Pins are not co-owners and must not be the design that makes “erase from inside callback” correct long-term.

Prefer: signal → parent drops **after** dispatch returns (or on the next tick on the owner’s thread).

## Affinity

Destroy on the **owner’s thread**:

| Kind | Typical owner affinity |
|------|------------------------|
| Rml / SDL / `CallMediaEngine` device teardown | UI |
| `ChannelSession` / mux / PeerLink | IO (`MeshPump`) |
| SQLite stores | whatever opened them — close store before wiping files |

Post teardown to the owner rather than destroying from a worker/callback on another thread. Calls chrome: [CALLS.md](CALLS.md) (never TearDown SDL on a worker).

## No use-after-erase

Copy fields out of a container element, then erase by id/key. Never touch the object (or a reference into it) after the owning container has destroyed it.

## Observers

Callbacks and listeners are **non-owning** (`weak_ptr`, raw under a documented parent lifetime, or id + lookup). Do not form strong cycles from child → parent that let the child drop the parent.

## Where this shows up

| Area | Parent | Child |
|------|--------|-------|
| App composition | `Application` / Hub | services, presenters ([RUNTIME_COMPOSITION](RUNTIME_COMPOSITION.md)) |
| Calls | `CallStack` / UI path | SDL / SFU / ringtone |
| Mesh | `PeerLinkManager` → link/mux → L4 slot | `ChannelSession` (A027) |
| Tests | gtest fixture | stores, then `remove_all` ([TEST_STRATEGY](../ops/TEST_STRATEGY.md#unit-test-conventions)) |

## Review checklist

- [ ] Who is the durable owner? Is destroy only on that path?
- [ ] Can a callback drop the last owner before it returns?
- [ ] Is teardown posted to the correct affinity?
- [ ] Any `erase` / `reset` after which code still uses the object?

## Stop / join before destroy (IStoppable)

Long-lived thread owners follow the same stop contract documented in [THREADING.md](THREADING.md):

```text
RequestStop(gen) → Drain(deadline) → Join(deadline) → destroy
```

Do **not** destroy (or `unique_ptr::reset`) an object while a detached / still-running worker may touch it — on join timeout, **leak until process exit** (`release()`), matching `MeshHost` / `ThreadRuntime` shutdown.

Primary owners: `CallStack`/`CallMediaBridge`, `ConversationsHub`/`MeshHost`, `AppRuntime`, LAN mDNS, `ILocalNotifier`, `CallRingtone`.

## Lifetime (ticket + Invalidate)

When a parent must post work that captures raw `this` / `Impl*` onto IO (or another mailbox) and teardown can race the queue, use `pp::Lifetime` (pp-cpp-common [`common/Lifetime.h`](https://github.com/people-post/pp-cpp-common/blob/main/include/common/Lifetime.h); pp-ledger guards its hosted servers with the same type):

| Do | Do not |
|----|--------|
| `Lifetime::Post` / `Bind` (exclusive post API) | Capture raw `this` into unguarded `PostToIo` |
| `Invalidate()` on Stop / Clear / AbortInflight | Yank tasks out of the queue |
| Sync Abort under lock when Finish would touch a destroyed owner | `PostIo(raw Impl*)` past Stop that still runs Finish into the owner |

`Invalidate` only bumps a generation; already-queued callbacks no-op when their snap no longer matches. New posts after Invalidate capture the new snap and keep working until the next Invalidate. Destroying the `Lifetime` invalidates too. `InvalidateAndWait` also waits for a bound call running on another thread — for an owner stopped from a thread other than the one its bound work runs on.

**Abort vs lifetime tickets (Amp L4):** The circuit and media_relay servers / client coordinators keep two `Lifetime`s — `deferred` for `PostIo` (Invalidate on AbortInflight) and `lifetime` for IoTick / protocol / PeerConnected (Invalidate only on Stop). Mid-life Abort must not poison ticks still needed while Started. `CallMediaLegCoordinator` uses `weak_ptr(Impl)` for ticks/handlers instead of a lifetime ticket.

**Teardown vs mid-life:** `Lifetime` is for owners that stop or abort **mid-life** on their own strand. Teardown of the messaging graph (quit, profile reset) is covered centrally by the `AppRuntime` gate — [THREADING.md § Teardown quiesce](THREADING.md#teardown-quiesce); do not add per-owner gates for it.

### Whitelist (who may capture raw self via Lifetime)

| Owner | Notes |
|-------|--------|
| Amp L4 coordinators (`CircuitRelayServer` / `CircuitClientCoordinator`, `MediaRelayServer` / `MediaRelayClientCoordinator`, `CallMediaLegCoordinator`) | `PostIo` → `deferred`; circuit/media-relay also `lifetime` for ticks/handlers; call-media uses `weak_ptr` for ticks |
| Amp protocols (`AmpPunchCoordinator`, `DialBackServer`, `AmpDhtProtocol`, `AmpDirectoryProtocol`) | Protocol-handler `Bind`; Invalidate on Stop |
| Amp transports — conversations (`AmpDirectChatTransport`, `AmpChatHistoryTransport`, `AmpChatBlobTransport`) and broadcast (`AmpBroadcastTransport`, `AmpPeerAnnounceTransport`, in `BroadcastServices`) | Protocol-handler `Bind`; Invalidate on Stop |
| `MeshConnectivity` / `CircuitRendezvousCoordinator` | OnRelayChosen (connectivity); reserve / park / repark cbs (rendezvous); `InvalidateAsyncOps` → `Invalidate` at mesh stop and Clear (a pending park answers `false` at once: its deadline was a timer on the stopping runtime) |
| `AmpCircuitHopReach` | AbortPending Invalidates; EnsureViaCircuit / punch cbs check Alive |
| `CallSessionManager` intents | `intents_self_` (dtor Invalidates): the posted answerer-kick retry and media restarts check Alive |

Everything else: prefer parent-only destroy + sync Abort, `shared_ptr`/`weak_ptr` pins for dispatch, or finish callbacks that do **not** capture the owner. Do not spread raw-`this` posts outside this whitelist without updating this table.
