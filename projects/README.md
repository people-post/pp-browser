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
| **Archive** | `git mv` the folder to [`_archive/`](_archive/README.md), fix its links, move its row to **Done / archived** below | When delivery ends; enduring facts already in `docs/`; its ids stay valid (CR025) |

Promote **outcomes** (must/behavior/version fields). Keep **rationale** in ADRs. Do not leave shipped wire schemas only under `projects/` (example: [WIRE_SCHEMAS.md](../docs/contracts/WIRE_SCHEMAS.md) — stub remains under chat-storage).

## Active projects

One-line intent only. **Phase detail → each folder’s CURRENT_STATE.md.**

| Project | Intent |
|---------|--------|
| [ai-centric-interface](ai-centric-interface/) | Intent taxonomy, agency, planner/tools |
| [qr-scanning](qr-scanning/) | In-app friend QR scanning and reusable scan entry |
| [chat-storage-and-memory](chat-storage-and-memory/) | SQLite threads, relay/history, memory/compaction |
| [multi-relay](multi-relay/) | Users on several relays: inboxes by Account ID, the relay list in the account card, cross-relay delivery |
| [platform-safety-limits](platform-safety-limits/) | Non-chat limits (LLM HTTP, profile JSON, MCP, parsers) |
| [e2e-message-crypto](e2e-message-crypto/) | Message AEAD / key UX residuals (contracts promoted) |
| [push-notifications](push-notifications/) | FCM wake + local alerts |
| [i18n](i18n/) | EN + zh-Hans UI language |
| [pricing](pricing/) | Initiation floor + media quote gates |
| [p2p-mesh](p2p-mesh/) | Mesh deepen, relay scope, name directory / pre-chain |
| [p2p-av-calls](p2p-av-calls/) | Voice-first call media on mesh |
| [call-path-resilience](call-path-resilience/) | Calls survive link / network changes; mobility-aware path policy |
| [audio-resilience](audio-resilience/) | Test the class of OS / hardware audio failures: disruptions × moments × session owner against five invariants (hearing measured, an emulated phone audio system); planned, starts after the ongoing projects |
| [capability-repos](capability-repos/) | Capability boundaries in the tree (gated); one capabilities repo when a second app picks them |
| [peer-scoped-broadcast](peer-scoped-broadcast/) | Peer announce feeds + live broadcast (media tree scale = Spine F) |
| [social-posts](social-posts/) | Public posts: signed hash-chained author logs, claimed → witnessed → anchored time |
| [media-client-layers](media-client-layers/) | Neutral reach / relay-attach / pipeline / device layers; broadcast as a sibling feature of calls |
| [hard-lab](hard-lab/) | Docker/netns forced-hop + impairment/discovery scenarios (Wave 1–3 partial scaffold) |
| [network-status-chrome](network-status-chrome/) | Desktop status bar cluster + popover |
| [multi-device-account](multi-device-account/) | Account ID, shared DEK, link-device |
| [adp](adp/) | The app's move onto the Amp stack (mesh underlay); the stack's own docs are in [pp-cpp-amp](https://github.com/people-post/pp-cpp-amp/blob/main/README.md#docs) |
| [feature-layer-reorg](feature-layer-reorg/) | `app → feature → domain → …` cleanup |
| [group-chat](group-chat/) | Group threads: signed roster, member check, mentions (G015) |
| [content-cas](content-cas/) | Private/public CAS realms; library / pin / Node-gated catalog use cases (C013) |
| [support-chat](support-chat/) | Support channel notes |
| [privacy](privacy/) | What peers, relays, servers and logs learn about a user, and the controls that bound it (first: who gets our IP) |
| [task-scopes](task-scopes/) | Every posted task owned by a scope tied to its owner's lifetime (own sequence, close on it); teardown owner by owner; proposed, after release 1 |
| [release-compatibility](release-compatibility/) | Release mode: every app, pp-node and relay in the supported range works with every other; format changes by the range rule; one main, a branch per release |
| [profile-model](profile-model/) | What a device keeps for whom: network → account → device profile → per-backend and per-relay state; backends as service tables; the tester-app guard on the network |

## Moved to another repo

A stub per file stays here, so old links keep working ([CR025](capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs); where every repo's docs are: [docs/README.md § Where docs live](../docs/README.md#where-docs-live)).

| Project | Now in |
|---------|--------|
| [media-hop-reachability](https://github.com/people-post/pp-cpp-mesh/tree/main/docs/projects/media-hop-reachability) | pp-cpp-mesh `docs/projects/` — circuit hop dial, punch, rendezvous; SoftMigrate consume |

## Done / archived

Delivery ended; use **docs/** for normative refs. Folders are kept in [`_archive/`](_archive/README.md) for their decisions and history (their ids are cited); relay-blob-upload is finished but stays in place, as a cross-repo program it moves to pp-specs after release 1 (CR025 step 2).

| Project | Stable refs |
|---------|-------------|
| [node-monitoring](_archive/node-monitoring/) | [NODE_METRICS](../docs/contracts/NODE_METRICS.md), [WINDOW_SHELL](../docs/ui/WINDOW_SHELL.md), [CALLS § Call media health](../docs/architecture/CALLS.md) |
| [thread-ownership](_archive/thread-ownership/) | [THREADING.md § Owner threads](../docs/architecture/THREADING.md#owner-threads), [CALLS.md](../docs/architecture/CALLS.md) |
| [liquid-glass](_archive/liquid-glass/) | [UI_DESIGN_SYSTEM — Floating Chrome](../docs/ui/UI_DESIGN_SYSTEM.md#compact-floating-chrome-materials), [WINDOW_SHELL](../docs/ui/WINDOW_SHELL.md) |
| [libp2p-pq-transport](_archive/libp2p-pq-transport/) | [AT_REST_ENCRYPTION](../docs/contracts/AT_REST_ENCRYPTION.md), [DATA_LAYOUT](../docs/contracts/DATA_LAYOUT.md), [LIBP2P_UPSTREAM](../docs/architecture/LIBP2P_UPSTREAM.md) |
| [at-rest-crypto](_archive/at-rest-crypto/) | [AT_REST_ENCRYPTION](../docs/contracts/AT_REST_ENCRYPTION.md), [DATA_LAYOUT](../docs/contracts/DATA_LAYOUT.md) |
| [relay-blob-upload](relay-blob-upload/) | [SERVICE_ENDPOINTS](../docs/contracts/SERVICE_ENDPOINTS.md), [WIRE_SCHEMAS](../docs/contracts/WIRE_SCHEMAS.md) |
