# Composition vocabulary — no upward concepts

**Tier:** architecture  
**Related:** [OWNERSHIP.md](OWNERSHIP.md) (parent-only destroy), [RUNTIME_COMPOSITION.md](RUNTIME_COMPOSITION.md) (who wires whom), [SRC_LAYOUT.md](SRC_LAYOUT.md) (layer edges), calls application [V048](../../projects/p2p-av-calls/DECISIONS.md#v048--composition-vocabulary-no-upward-concepts).

## Rule

**A lower peer must not embed codes, calls, or concepts from a higher peer.**

Composition roots (`Application`, `CallStack`, `MeshHost`, …) own lifetimes and **project** meaning upward. Sibling products under a root talk through **narrow ports that speak the consumer’s needs**, not the producer’s domain model.

Removing a raw pointer (`Foo*`) while ports still take `FooStatus` / `set_foo_phase(…)` is still upward coupling.

This is a **repo-wide** design convention (calls, mesh, UI↔functional, feature peers). Layer include bans stay in [SRC_LAYOUT.md](SRC_LAYOUT.md); destroy direction stays in [OWNERSHIP.md](OWNERSHIP.md).

## Litmus

Ask from the **lower** peer’s point of view, pretending the higher peer does not exist:

| Ask | Good port shape | Bad (upward vocabulary) |
|-----|-----------------|-------------------------|
| What do I need to proceed? | Boolean arming / admission, epoch / cancel gen, seat token | “Is Lifecycle Status `HopWaiting`?” |
| What do I report? | My own progress / events / outcomes | Writing the higher peer’s chrome enum |
| Who maps? | Composition root adapter | Lower peer `#include`s higher types “just for the enum” |

## Knowledge vs ownership

Ownership may be flat (siblings under one `unique_ptr` owner). **Dependency of meaning** still points down or sideways:

```text
Composition root
  ├── Higher product (chrome / policy / arming)
  ├── Mid product (session façade)
  └── Lower product (planner / engine / path)
        ports: needs + events only
              ↑
         root projects to higher
```

Prefer **“higher observes lower”** (progress callbacks, native phase) over **“lower drives higher”** (lower writes higher status enums).

## Ports

Ports are allowed and encouraged. They must:

1. Be installed by the composition root (or an owning façade the root already owns).
2. Name **capabilities the consumer needs**, not the sibling’s type name as the API (`allows_ops`, `cancel_gen`, `on_progress` — not `set_call_media_status(CallMediaStatus)`).
3. Stay null-guard friendly for unit tests (empty ports = permissive or no-op, documented).

### Port type ownership

| Piece | Lives where |
|-------|-------------|
| Port **struct** | Consumer header (the type that `Set*Ports` / stores the ports) |
| **`Make*` adapters** | Private methods on the composition root / owning façade that closes over producers (`CallStack::MakeHopArmingPorts`, `CallSessionManager::MakeSeatPorts`) |
| Free `Make*` + standalone `*Ports.{h,cpp}` | Avoid — duplicates the consumer contract and invites upward `#include`s |

Unit / compose tests act as mini-composers: define local helpers in the test TU (or empty ports), not shared free `Make*` in the feature library.

## Where this shows up

| Area | Higher | Lower | Typical leak |
|------|--------|-------|--------------|
| Calls | `CallLifecycle` Status / chrome | Topology / Bridge planners | *(cleared V048)* hop/direct arming ports; Stack projects |
| Calls | `CallSessionManager` façade | Topology / Workflow | Grab-bag host that re-exports Lifecycle |
| UI ↔ functional | Shell / presenters | Domain / feature engines | Functional code naming shell chrome types |
| Mesh | Host / L3 policy | L4 sessions | Session code owning host-wide phase enums |

## Review checklist

- [ ] If you delete the higher type’s header, does the lower peer still compile?
- [ ] Do ports use the lower peer’s vocabulary (needs / events) or the higher peer’s enums?
- [ ] Is mapping to chrome / product Status done only at the composition root?
- [ ] Are there dual writers of the same progress story (native phase **and** higher Status)?

## Stop short of

- Forcing every sibling into a deep ownership tree (flat under root is fine).
- Moving lower race/IO clusters into the higher chrome machine.
- Banning shared **neutral** contracts in `common/` (ids, wire DTOs both peers must name without owning lifecycle).
