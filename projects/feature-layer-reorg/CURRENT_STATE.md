# Feature / app reorg — current state

**Updated:** 2026-09-03  
**Phase:** f1+f2 peels landed → **f3 next**

## What already shipped (prior work)

- `src/base/` retired → `src/foundation/` + `src/domain/`
- Domain peers strictly independent; cross-need via `common/` + feature wiring
- Feature module libs acyclic: `settings → ai → messaging → ui → chat`
- Ports + app bridges break UI↔functional cycles ([UI_FUNCTIONAL_BOUNDARY.md](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md))
- Include guard: [`scripts/check_feature_includes.sh`](../../scripts/check_feature_includes.sh)
- **f0** project docs + **F006** (sure peels use existing peers, no new top-level domain peers)
- **f1+f2** sure peels into `domain/{messaging,people,mesh/reachability,ui}` (Hub still owns `unique_ptr`s)

## Snapshot (approx. non-test `.h`/`.cpp`)

| Area | Notes |
|------|-------|
| `feature/messaging` | Still hub/sync/calls; stores/PSK/reach helpers peeled |
| `feature/ui` | No longer includes messaging `ContactReachability`; uses `domain/people` |
| `domain/messaging` | + PSK store/coordinators, `CallMediaKeyStore`, epoch bump |
| `domain/people` | + reachability, brief route, profile icon fetch |
| `domain/mesh/reachability` | + `MobileEphemeralListenGate` |
| `domain/ui` | + `PeoplePickerLogic`, `CallConflictCopy` |

## Pain points (remaining)

1. Calls still under `feature/messaging` (SM) vs `feature/ui` (chrome) — f4.
2. `feature/ui` still a grab-bag — f5.
3. Inbox presentation leak / ChatController size — f6.
4. messaging → ai hard-includes `AgentSession`.
5. Cross-peer utils still blocked (`DirectoryShadowCache`, etc.).

## Next agent — start here

1. **f3:** `ChatHistoryResponder` identity peel → `domain/messaging`; `IDirectMessageClient` → `common/`.
2. Or start **f4** `feature/calls` extraction if preferred after reviewing file counts.
3. Do not add new domain peers without an ADR overriding [F006](DECISIONS.md#f006--sure-peels-use-existing-domain-peers-no-new-peers).

## Related docs still slightly stale

| Doc | Stale bit |
|-----|-----------|
| `RUNTIME_COMPOSITION.md` | Diagrams may still say `base/` |
| Feature README / CALLS | Prefer opportunistic path fixes when touching those areas |

Fix those opportunistically when touching layout.
