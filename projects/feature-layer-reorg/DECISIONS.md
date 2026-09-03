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
**Status:** accepted (path); top-level libs deferred

**Decision:** Nest under `feature/ui/shell/` and `feature/ui/contacts/` (same `pp_feature_ui`) before creating `pp_feature_shell` / `pp_feature_contacts`. Chat screen lives in `feature/ui/chat/` (no top-level `feature/chat`) per [F007](#f007--vocabulary--end-state-feature-names).

**Done (f5v1):** bands created; `pp_feature_chat` removed.

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

**End-state feature folders:**

```
feature/
  settings/
  ai/
  conversations/   # rename from today’s feature/messaging (hub + delivery)
  calls/           # call session (after f4 cycle break; f4v1 nested under messaging/calls/)
  shell/           # window host, chrome, gestures (from ui split)
  contacts/        # contacts + people-picker (from ui split)
  ui/              # shared presenters/ports; absorbs today’s ChatController (no top-level chat/)
```

| Today (legacy path) | End-state name | Role |
|---------------------|----------------|------|
| `feature/messaging` | `feature/conversations` | Conversations hub + delivery adapters |
| `feature/messaging` Call\* (banded) | `feature/calls` | Call **session** orchestration |
| `feature/chat` | **removed** as top-level | `ChatController` → `feature/ui` (or shell) with other presenters |
| `domain/messaging` | **keep** | Conversation/call **record & codec** engines (not the product hub) |
| `domain/media` | **keep** | Call **media** engines only |

**Explicit non-goals for naming:**

- Do **not** add `domain/calls` ([F006](#f006--sure-peels-use-existing-domain-peers-no-new-peers)).
- Do **not** keep top-level `feature/chat` once `conversations` exists.
- Do **not** rename `domain/messaging` to `domain/chat` (call control types stay in the messaging peer).

**Migration note:** Until renames ship, docs may say “`feature/messaging` = conversations (legacy path).” Class renames (`MessagingHub` → `ConversationsHub`) track the folder rename, not f4v1.

**Consequences:** f4/f5/f6 plan toward this map; promote into SRC_LAYOUT / `src/feature/README.md` when folders actually move.
