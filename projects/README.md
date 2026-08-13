# Ongoing projects

Work-in-progress design and implementation tracking for pp-browser. Unlike [`docs/`](../docs/) **contracts** and architecture (stable, versioned), these files are **living project notes**: they change as work proceeds, track checkboxes, and record decisions **before** normative text lands in stable reference docs.

**Doc map:** [`docs/README.md`](../docs/README.md) — Architecture | Contracts | Product/UI | Ops.  
**Compatibility policy:** [`docs/contracts/COMPATIBILITY.md`](../docs/contracts/COMPATIBILITY.md).

## How to use (humans and agents)

1. Open the project folder for the feature you are working on.
2. Read **DESIGN.md** (complete spec) and **CURRENT_STATE.md** (today) before coding.
3. Record rollout choices in the relevant project **DECISIONS.md** (and promote to [`docs/contracts/`](../docs/contracts/) when behavior ships). Do not keep a separate pending checklist.
4. Pick tasks from **PHASES.md** (ordering only); for batch pre-release delivery, follow **PHASES § Agent batch delivery** in [chat-storage](chat-storage-and-memory/PHASES.md#agent-batch-delivery-order) and [e2e](e2e-message-crypto/PHASES.md#agent-batch-delivery-order).
5. Mark items done in the same PR that implements them.
6. Log non-obvious choices in **DECISIONS.md** (date + rationale).
7. When a phase ships, update the status line in the project **README.md**.

## Lifecycle: promote → freeze → archive

| Stage | Where | Rule |
|-------|--------|------|
| Explore | `DESIGN.md`, open ADRs | Churn expected |
| Ship | `PHASES.md`, `CURRENT_STATE.md` | Checkboxes match code |
| **Promote** | [`docs/contracts/`](../docs/contracts/) (and other tiers as appropriate) | Wire, disk, HTTP, crypto, compat policy — **one canonical file**; same release window as the ship |
| **Freeze** | Project `DECISIONS.md` | Mark outcomes **superseded by** `docs/…`; do not edit normative tables in two places |
| **Archive** | Delete or clearly mark project folder done | When delivery ends; enduring facts already in `docs/` |

Promote **outcomes** (must/behavior/version fields). Keep **rationale** in ADRs. Do not leave shipped wire schemas only under `projects/` (example: [WIRE_SCHEMAS.md](../docs/contracts/WIRE_SCHEMAS.md) was promoted from chat-storage).

## Active projects

| Project | Status | Summary |
|---------|--------|---------|
| [ai-centric-interface](ai-centric-interface/) | **d0 design** — v1 next | Intent taxonomy (10 acts), agency, planner/tools — thin path for every act first — [CURRENT_STATE](ai-centric-interface/CURRENT_STATE.md) |
| [chat-storage-and-memory](chat-storage-and-memory/) | **Waves 1–2 + v3 core done** — v4 next | SQLite, v1 relay, tier shells, memory/compaction — see [CURRENT_STATE § Next agent](chat-storage-and-memory/CURRENT_STATE.md#next-agent--start-here) |
| [platform-safety-limits](platform-safety-limits/) | Planning | LLM HTTP, profile JSON, MCP, parser output — non-chat limits |
| [e2e-message-crypto](e2e-message-crypto/) | **c1 done** — c2 after chat v6 | `base/crypto` + vectors; AEAD on wire in c2 — [CURRENT_STATE](e2e-message-crypto/CURRENT_STATE.md) |
| [push-notifications](push-notifications/) | **Wave 1 done** | Owned Brief FCM wake + local alerts; alerts ≠ sync — [CURRENT_STATE](push-notifications/CURRENT_STATE.md) |
| [i18n](i18n/) | **i1–i6 landed** — widen i5 | EN + zh-Hans UI language; Settings picker (sheet on mobile) — [CURRENT_STATE](i18n/CURRENT_STATE.md) |
| [liquid-glass](liquid-glass/) | **Done** | Floating Chrome on compact shell — [stable docs](../docs/ui/UI_DESIGN_SYSTEM.md#compact-floating-chrome-materials) |
| [pricing](pricing/) | **P001 first slice** | Initiation floor + media quote gates (no payment rails) — [DECISIONS](pricing/DECISIONS.md) |
| [p2p-mesh](p2p-mesh/) | **nf + n4-media done** — N022 invest; **N023 relay scope** spec | Libp2p deepen; scope escalation; HTTP settle preferred — [RELAY_SCOPE](p2p-mesh/RELAY_SCOPE.md) |
| [p2p-av-calls](p2p-av-calls/) | **V026** libp2p media — **m1** next | Voice-first on libp2p; WebRTC legacy teardown after m1 — [CURRENT_STATE](p2p-av-calls/CURRENT_STATE.md) |
| [media-hop-reachability](media-hop-reachability/) | **L1–L3 shipped** — L4 consume next; L3.5 multi-hop planned | In-libp2p dial-by-PeerId; SoftMigrate consumes — [DESIGN](media-hop-reachability/DESIGN.md) |
| [network-status-chrome](network-status-chrome/) | **s1 landed** — s2 next | Desktop status bar cluster + hybrid popover (mesh/reach/help/load) — [DESIGN](network-status-chrome/DESIGN.md) |
| [multi-device-account](multi-device-account/) | **m4b paste path landed** — unlink / `endpoints[]` next | Account ID on wire; shared DEK + first-run link-device — [DESIGN](multi-device-account/DESIGN.md) |
