# Thread ownership

> **Done (2026-09-27, t1–t4).** Normative rules live in [THREADING.md § Owner threads](../../docs/architecture/THREADING.md#owner-threads) (roles 7–8, T004 wait direction, completions instead of mesh waits) and [CALLS.md](../../docs/architecture/CALLS.md). This folder is history: DESIGN (the starting picture), PHASES, DECISIONS (rationale).

Give every piece of mutable product state exactly one owning thread, and make threads talk by message. Four owners: **Mesh I/O** (Amp transport + protocol engines, as today), **Connectivity** (reach policy), **Media sessions** (calls + broadcast control), **UI** (presentation). Blocking work stays on the worker pool; real-time media threads stay as they are.

Started from the call port-rebind race (TSan: `CallSessionManager::BindWorkflowHostPorts` rewriting ports on UI / mesh bring-up while call workers invoke them) — a symptom of call state being touched from five threads with no owner.

| File | Content |
|------|---------|
| [DESIGN.md](DESIGN.md) | Owners, rules, what moves where, test drain model |
| [DECISIONS.md](DECISIONS.md) | T001… |
| [PHASES.md](PHASES.md) | t1 primitive → t2 media sessions → t3 connectivity → t4 UI snapshots |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Status |

Related: [THREADING.md](../../docs/architecture/THREADING.md) (normative roles; this project adds the ownership rules), [media-client-layers](../media-client-layers/) (l7 found the race; l8 made `MeshMediaPlane` the connectivity owner's first object).
