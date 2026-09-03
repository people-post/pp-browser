# Feature / app reorg — decisions

Living ADRs for **method and layout**. Promote durable layout rules into [SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md) and [`src/feature/README.md`](../../src/feature/README.md); mark superseded here.

---

## F001 — Sure-things-first; revisable North Star

**Date:** 2026-09-03  
**Status:** accepted

**Context:** Foundation/domain path migration is done. Feature/app still need reorg, but the ideal `feature/*` folder map is not yet proven. Premature folder renames risk large diffs without ownership clarity.

**Decision:**

1. Lock the **delivery method**: peel clear domain engines first; structural folder splits only after ownership is evidenced; each phase must leave CI green.
2. Keep a **working** North Star in [NORTH_STAR.md](NORTH_STAR.md) — revise freely.
3. Lock individual map slices only via later ADRs (F00x), then promote to SRC_LAYOUT.

**Consequences:** Agents should not open mega “final layout” PRs. Prefer CANDIDATES confidence tags. Open questions stay open until peels teach.

---

## F002 — Confidence tags for moves

**Date:** 2026-09-03  
**Status:** accepted

**Decision:** Every candidate in [CANDIDATES.md](CANDIDATES.md) carries one tag:

| Tag | Meaning | Allowed action |
|-----|---------|----------------|
| **sure** | Single peer / foundation+common; passes domain litmus | Move in f1–f3 |
| **likely** | Domain-shaped but mild coupling; short peel first | Move after noted peel |
| **blocked** | Cross-peer or orchestration | Stay feature or need `common` first |
| **structural** | Folder/CMake split, no behavior change | Only after related **sure** peels |

Do not promote **blocked** → domain to “clean the folder.”

---

## F003 — App stays composition root

**Date:** 2026-09-03  
**Status:** accepted

**Decision:** Lifetimes and cross-controller wiring remain in `src/app/`. Cleanup means **named wirers** and thin bridges — not moving orchestration into random feature headers, and not a new DI framework.

**Consequences:** `Application` may stay large in ownership terms; line-count reduction comes from splitting TUs / helpers, not from abandoning the composition root.

---

## F004 — Calls home: nested band first, then top-level

**Date:** 2026-09-03  
**Status:** accepted (path); top-level lib still deferred

**Context:** Separating call session from conversations is desired, but `CallSessionManager` ↔ `MeshMessagingService` would cycle if `pp_feature_calls` and `pp_feature_messaging` both exist today.

**Decision:**

1. **f4v1:** nest under `feature/messaging/calls/` (same `pp_feature_messaging` target) — discoverability only.
2. **Later:** top-level `feature/calls/` + `pp_feature_calls` only after ownership is one-way (call session uses delivery ports; conversations hub does **not** own `CallStack`).
3. End-state name for the module is **`calls`** (call **session**), not `av` or `media` — see [F007](#f007--vocabulary--end-state-feature-names).

---

## F005 — Shell / contacts: nested bands first

**Date:** 2026-09-03  
**Status:** accepted (path); **amended by [F008](#f008--gui-layer-above-feature)** (end-state home is `src/gui/`, not top-level `feature/shell`)

**Decision:** Nest under `feature/ui/shell/` and `feature/ui/contacts/` (same `pp_feature_ui`) as an intermediate discoverability step. Chat screen lives in `feature/ui/chat/` (no top-level `feature/chat`) per [F007](#f007--vocabulary--end-state-feature-names).

**Done (f5v1):** bands created; `pp_feature_chat` removed.

**Amendment:** Do **not** promote those bands to `pp_feature_shell` / `pp_feature_contacts`. Next structural home is a top-level **`src/gui/`** layer ([F008](#f008--gui-layer-above-feature)).

---

## F006 — Sure peels use existing domain peers (no new peers)

**Date:** 2026-09-03  
**Status:** accepted

**Context:** f1–f3 will move stores/helpers out of `feature/`. Question: add `domain/calls`, `domain/psk`, etc., or reshape peer trees first?

**Decision:**

1. **Do not add top-level domain peers** for sure peels. Keep the peer set: `people`, `messaging`, `media`, `mesh`, `net`, `ui`, `ai`.
2. **Land files in the peer that already owns the sibling types** (flat unless that peer already nests).
3. **Do not invent `domain/calls`.** Call session types/stores already live in `domain/messaging`; capture/playback stays in `domain/media`. A new calls peer would either link messaging (banned) or orphan `CallSessionStore`.
4. **Optional internal subfolders** under `domain/messaging/` (e.g. `call/`, `psk/`, `thread/`) are a **later** readability pass — not a prerequisite for f1. Prefer flat drops next to existing `Call*`, `Psk*`, `Sqlite*` files first.
5. **`CallMediaKeyStore` → `domain/messaging`**, not `domain/media` — it is vault/SQLite next to `CallSessionStore`; `domain/media` is engine/codec/OS capture.

**Homes for sure candidates:**

| Candidate | Home |
|-----------|------|
| `SqlitePskSessionStore`, PSK/epoch coordinators | `domain/messaging/` (flat) |
| `CallMediaKeyStore` | `domain/messaging/` (flat) |
| `ContactReachability`, `PeerBriefRoute`, `ProfileIconFetchUtil` | `domain/people/` (flat) |
| `MobileEphemeralListenGate` | `domain/mesh/reachability/` (nested — peer already nests) |
| `PeoplePickerLogic`, `CallConflictCopy` | `domain/ui/` (flat) |

**Consequences:** f1–f3 are path/CMake moves into known peers. Messaging stays a large flat folder for now; revisit internal banding only if navigation pain remains after peels.

---

## F007 — Vocabulary + end-state feature names

**Date:** 2026-09-03  
**Status:** accepted

**Context:** “Messaging” and “call” are overloaded (transport vs product; media plane vs session). Top-level `feature/chat` next to a future `conversations` module would keep the synonym problem.

**Vocabulary (docs / comments):**

| Term | Means |
|------|--------|
| **Delivery** | Envelope/stream send–receive (relay, Amp chat channel, inbox) |
| **Conversations** | Threads, sync, attachments, groups, PSK-for-chat — product hub |
| **Call session** | Ring → accept → keys → topology → leave |
| **Call media** | Audio/video frames, codecs, legs (`domain/media` + mesh L4) |
| **Chat** (UI copy) | User-facing label for the conversation screen — **not** a layer folder name |

Slogan: **conversations ∥ call session**, both use **delivery**; call session drives **call media**.

**End-state feature folders** (product orchestration only — presenters live under **`gui/`**, [F008](#f008--gui-layer-above-feature)):

```
feature/
  settings/        # config apply + section handlers (not Me presenters)
  ai/              # agent session, turn pipeline, tools, bindings
  conversations/   # rename from today’s feature/messaging (hub + delivery)
  calls/           # call session (after f4 cycle break; f4v1 nested under messaging/calls/)
```

| Today (legacy path) | End-state name | Role |
|---------------------|----------------|------|
| `feature/messaging` | `feature/conversations` | Conversations hub + delivery adapters |
| `feature/messaging` Call\* (banded) | `feature/calls` | Call **session** orchestration |
| `feature/chat` / `feature/ui/*` | **`src/gui/`** ([F008](#f008--gui-layer-above-feature)) | Presenters / shell — not feature modules |
| `domain/messaging` | **keep** | Conversation/call **record & codec** engines (not the product hub) |
| `domain/media` | **keep** | Call **media** engines only |
| `domain/ui` | **keep** | Non-Rml presentation **policy** (name stays; not the GUI layer) |

**Explicit non-goals for naming:**

- Do **not** add `domain/calls` ([F006](#f006--sure-peels-use-existing-domain-peers-no-new-peers)).
- Do **not** keep top-level `feature/chat` once `conversations` exists.
- Do **not** rename `domain/messaging` to `domain/chat` (call control types stay in the messaging peer).
- Do **not** name the product UI layer `src/ui/` — that collides with `domain/ui` ([F008](#f008--gui-layer-above-feature)).

**Migration note:** Until renames ship, docs may say “`feature/messaging` = conversations (legacy path).” Class renames (`MessagingHub` → `ConversationsHub`) track the folder rename, not f4v1. Today’s `feature/ui/**` is the staging area for the `gui` lift.

**Consequences:** f4/f5/f6 plan toward conversations/calls; **f7** lifts presenters into `src/gui/`. Promote into SRC_LAYOUT when folders actually move.

---

## F008 — `gui` layer above feature

**Date:** 2026-09-03  
**Status:** accepted (target layout); path not shipped yet

**Context:** [UI_FUNCTIONAL_BOUNDARY.md](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md) treats Rml presenters and functional hubs as two systems. Nesting shell/contacts/chat under `feature/ui/` (f5) improved discoverability but left presenters as a **peer** of messaging/ai in the same layer while depending on them — wrong mental model and a name clash with `domain/ui` (non-Rml policy).

**Decision:**

1. Add a product layer **`src/gui/`** (includes `gui/…`, target `pp_gui` / bands) **above feature**:

   ```
   app → gui → feature → domain → foundation → common
   ```

2. **Name is `gui`, not `ui`.** Reserve `domain/ui` for pure presentation policy (copy, picker logic, no Rml controllers). Do not rename `domain/ui` in this ADR.

3. **`gui` owns** all product Rml/SDL UI buildings: shell, chat screen, contacts/people-picker, settings presenters, call chrome, shared chrome helpers (badges, flow, data-model host, …).

4. **`feature` owns** headless-capable orchestration only (conversations hub, call session, settings apply, agent). **Ban** `feature → gui` includes/links once the lift ships.

5. **`app` stays** composition root ([F003](#f003--app-stays-composition-root)): owns hub + GUI lifetimes; named wirers bind `gui → feature` facades.

6. **Migration:** mechanical lift `feature/ui/**` → `src/gui/**` (keep nested bands `shell/`, `contacts/`, `chat/`, …); retire `pp_feature_ui`; update include guards. One aggregate `pp_gui` first; split libs later only if cycles allow.

7. **Supersedes** the F005/F007 idea of end-state top-level `feature/shell` / `feature/contacts` / residual `feature/ui`. Those become **bands under `gui/`**.

**Explicit non-goals:**

- No extra `presentation` / `view` / `widgets` layers yet.
- No top-level `src/shell` between app and gui.
- No simultaneous messaging→conversations rename in the lift PR.

**Consequences:** Next structural phase is **f7** ([PHASES.md](PHASES.md)). Until then, paths remain `feature/ui/**`; docs say “staging for `gui`.”
