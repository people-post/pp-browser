# Ongoing projects

Work-in-progress design and implementation tracking for pp-browser. Unlike [`docs/`](../docs/) **contracts** and architecture (stable, versioned), these files are **living project notes**: they change as work proceeds, track checkboxes, and record decisions **before** normative text lands in stable reference docs.

**Doc map:** [`docs/README.md`](../docs/README.md) — Architecture | Contracts | Product/UI | Ops.  
**Agent orientation:** [`AGENTS.md`](../AGENTS.md) (no phase status there).  
**Compatibility policy:** [`docs/contracts/COMPATIBILITY.md`](../docs/contracts/COMPATIBILITY.md).

## Authority (status vs stable)

| Question | Source of truth |
|----------|-----------------|
| What shipped behavior / wire / disk / HTTP must be | [`docs/`](../docs/README.md) |
| What phase is next / what landed this week | That project’s **`CURRENT_STATE.md` only** |
| Why we chose X | Project **`DECISIONS.md`** (rationale); outcomes may be **superseded by** a docs contract |
| Coarse “is this folder still active?” | Tables below (may lag a day; never duplicate into `AGENTS.md`) |

Do **not** edit phase status into `AGENTS.md` or keep a second editable normative schema under `projects/` after promotion.

## How to use (humans and agents)

1. Open the project folder for the feature you are working on.
2. Read **DESIGN.md** (complete spec) and **CURRENT_STATE.md** (today) before coding.
3. Record rollout choices in the relevant project **DECISIONS.md** (and promote to [`docs/contracts/`](../docs/contracts/) when behavior ships).
4. Pick tasks from **PHASES.md** (ordering only); for batch pre-release delivery, follow **PHASES § Agent batch delivery** in [chat-storage](chat-storage-and-memory/PHASES.md#agent-batch-delivery-order) and [e2e](e2e-message-crypto/PHASES.md#agent-batch-delivery-order).
5. Mark items done in the same PR that implements them; update **CURRENT_STATE.md** in that PR.
6. Log non-obvious choices in **DECISIONS.md** (date + rationale).
7. When a phase ships, optionally refresh the one-line summary in this index — do not require updating `AGENTS.md`.

## Lifecycle: promote → freeze → archive

| Stage | Where | Rule |
|-------|--------|------|
| Explore | `DESIGN.md`, open ADRs | Churn expected |
| Ship | `PHASES.md`, `CURRENT_STATE.md` | Checkboxes match code |
| **Promote** | [`docs/contracts/`](../docs/contracts/) (and other tiers as appropriate) | Wire, disk, HTTP, crypto, compat policy — **one canonical file**; same release window as the ship |
| **Freeze** | Project `DECISIONS.md` | Mark outcomes **superseded by** `docs/…`; do not edit normative tables in two places |
| **Archive** | Move row to **Done / archived** below (folder may remain for ADR history) | When delivery ends; enduring facts already in `docs/` |

Promote **outcomes** (must/behavior/version fields). Keep **rationale** in ADRs. Do not leave shipped wire schemas only under `projects/` (example: [WIRE_SCHEMAS.md](../docs/contracts/WIRE_SCHEMAS.md) — stub remains under chat-storage).

## Active projects

One-line intent only. **Phase detail → each folder’s CURRENT_STATE.md.**

| Project | Intent |
|---------|--------|
| [ai-centric-interface](ai-centric-interface/) | Intent taxonomy, agency, planner/tools |
| [chat-storage-and-memory](chat-storage-and-memory/) | SQLite threads, relay/history, memory/compaction |
| [platform-safety-limits](platform-safety-limits/) | Non-chat limits (LLM HTTP, profile JSON, MCP, parsers) |
| [e2e-message-crypto](e2e-message-crypto/) | Message AEAD / key UX residuals (contracts promoted) |
| [push-notifications](push-notifications/) | FCM wake + local alerts |
| [i18n](i18n/) | EN + zh-Hans UI language |
| [pricing](pricing/) | Initiation floor + media quote gates |
| [p2p-mesh](p2p-mesh/) | Mesh deepen, relay scope, name directory / pre-chain |
| [p2p-av-calls](p2p-av-calls/) | Voice-first call media on mesh |
| [peer-scoped-broadcast](peer-scoped-broadcast/) | Peer announce feeds + live broadcast (media tree scale = Spine F) |
| [media-hop-reachability](media-hop-reachability/) | Circuit hop dial / SoftMigrate consume |
| [hard-lab](hard-lab/) | Docker/netns forced-hop + impairment scenarios (design) |
| [network-status-chrome](network-status-chrome/) | Desktop status bar cluster + popover |
| [multi-device-account](multi-device-account/) | Account ID, shared DEK, link-device |
| [adp](adp/) | ADP L1 + AMP stack (mesh underlay) |
| [feature-layer-reorg](feature-layer-reorg/) | `app → feature → domain → …` cleanup |
| [group-chat](group-chat/) | Group threads (design / follow-on) |
| [content-cas](content-cas/) | Private/public CAS realms; big-bang attachment cutover; public publish |
| [support-chat](support-chat/) | Support channel notes |

## Done / archived

Delivery ended; use **docs/** for normative refs. Folders kept for ADR / history unless deleted later.

| Project | Stable refs |
|---------|-------------|
| [liquid-glass](liquid-glass/) | [UI_DESIGN_SYSTEM — Floating Chrome](../docs/ui/UI_DESIGN_SYSTEM.md#compact-floating-chrome-materials), [WINDOW_SHELL](../docs/ui/WINDOW_SHELL.md) |
| [libp2p-pq-transport](libp2p-pq-transport/) | [AT_REST_ENCRYPTION](../docs/contracts/AT_REST_ENCRYPTION.md), [DATA_LAYOUT](../docs/contracts/DATA_LAYOUT.md), [LIBP2P_UPSTREAM](../docs/architecture/LIBP2P_UPSTREAM.md) |
| [at-rest-crypto](at-rest-crypto/) | [AT_REST_ENCRYPTION](../docs/contracts/AT_REST_ENCRYPTION.md), [DATA_LAYOUT](../docs/contracts/DATA_LAYOUT.md) |
| [relay-blob-upload](relay-blob-upload/) | [SERVICE_ENDPOINTS](../docs/contracts/SERVICE_ENDPOINTS.md), [WIRE_SCHEMAS](../docs/contracts/WIRE_SCHEMAS.md) |
