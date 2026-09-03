# Working North Star — feature / app / gui

**Status:** end-state names in [F007](DECISIONS.md#f007--vocabulary--end-state-feature-names) + **`gui` layer** in [F008](DECISIONS.md#f008--gui-layer-above-feature); paths migrate in phases  
**Promote** folder renames into [SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md) when they ship.

Layer north star:

> `common` names the shared language; `foundation` implements the shared kernel; `domain` implements independent product capabilities; `feature` composes them **without Rml**; **`gui` presents** them; `app` constructs the graph.

```
app → gui → feature → domain → foundation → common
```

(`domain/ui` stays a **domain peer** for non-Rml presentation policy — not the GUI layer.)

---

## Vocabulary ([F007](DECISIONS.md#f007--vocabulary--end-state-feature-names))

| Term | Means |
|------|--------|
| **Delivery** | Envelope/stream send–receive |
| **Conversations** | Threads, sync, attachments, groups — product hub |
| **Call session** | Ring → accept → keys → topology → leave |
| **Call media** | A/V frames, codecs, legs |
| **Chat** | User-facing screen label only — **not** a layer folder name |
| **GUI** | Product Rml/SDL UI layer (`src/gui/`) — presenters, shell, chrome |

**conversations ∥ call session** → both use **delivery**; call session drives **call media**.

---

## Domain peer set ([F006](DECISIONS.md#f006--sure-peels-use-existing-domain-peers-no-new-peers))

| Peer | Role |
|------|------|
| `messaging` | Conversation/call **record & codec** engines (keep name; not the product hub) |
| `people` | Contacts/identity helpers |
| `mesh` | Amp host, reachability, L4 call-media / media-relay |
| `media` | Call-media capture/playback/codecs only |
| `ui` | Pure presentation **policy** (no Rml controllers) — **not** `src/gui` |
| `net` / `ai` | Unchanged |

Do **not** add `domain/calls`. Do **not** rename this peer to collide with the GUI layer.

---

## Litmus

| Layer | Put it here when… | Not when… |
|-------|-------------------|-----------|
| **domain** | Owns one store, codec, client, or pure policy | Coordinates several peers or owns Rml lifecycle |
| **feature** | Coordinates workflows / hubs / sessions (headless-capable) | Owns documents, data models, or chrome gestures |
| **gui** | Owns a screen, shell chrome, Rml bind, or UI-only helper | Hub/session engines, mesh, stores |
| **app** | Lifetimes + cross-surface wiring | Product policy or I/O engines |
| **common** | Second domain peer must compile against the name | Only one peer needs it |

**Cross-peer rule:** two of `{people, messaging, net, mesh, media, ui}` → feature or `common` contracts — never domain→domain.  
**Layer rule:** `feature` must not `#include "gui/…"` once the lift ships ([F008](DECISIONS.md#f008--gui-layer-above-feature)).

---

## End-state maps

### Feature (orchestration only)

```
feature/
  settings/        # config apply + section handlers
  ai/              # agent session, turn pipeline, tools, bindings
  conversations/   # TODAY: feature/messaging — hub, delivery, sync, groups, façade
  calls/           # call session — f4v1 nested as messaging/calls/; later top-level
```

### GUI ([F008](DECISIONS.md#f008--gui-layer-above-feature))

```
gui/               # TODAY staged as feature/ui/** — lift in f7
  shell/           # ShellHost, gestures, feedback, RmlMount, chrome ports
  contacts/        # Contacts + PeoplePicker
  chat/            # ChatController + screen helpers (no top-level feature/chat)
  call/            # CallController / call chrome (optional band)
  settings/        # SettingsController + Me sections (optional band)
  shared/          # BadgeAggregator, FlowCoordinator, DataModelHost, …
```

One aggregate `pp_gui` first; split libs later only if cycles allow.

### Legacy → end-state

| Today | End state | Notes |
|-------|-----------|-------|
| `feature/messaging` | `feature/conversations` | Rename when hub ownership is clean |
| `feature/messaging` Call\* | `feature/calls` | [F004](DECISIONS.md#f004--calls-home-nested-band-first-then-top-level): nest first |
| `feature/ui/**` (incl. shell/contacts/chat) | `src/gui/**` | [F008](DECISIONS.md#f008--gui-layer-above-feature); name **gui** not ui |
| `feature/chat` | removed | Absorbed into gui/chat (via f5 staging) |
| `domain/ui` | keep | Policy peer; not the GUI layer |

Intended link order (end state):

```
feature:  settings → ai → conversations → calls
gui:      → feature (aggregate or bands); never the reverse
app:      → gui + feature
```

Ownership: **app** owns `ConversationsHub` and `CallStack` separately; conversations does **not** own call session long-term. **App** owns GUI controllers; GUI binds feature facades through ports.

---

## Working app map

- Keep **ownership** in `Application` (composition root) ([F003](DECISIONS.md#f003--app-stays-composition-root)).
- Prefer **named wirers** (`WireShell`, `WireConversations`, `WireCalls`, …).
- Keep thin `*Bridge` helpers for cycle breaks / chrome apply.

---

## Still open

1. Exact band names under `gui/` (`call/` vs residual shared; settings section home).
2. Whether Amp *chat* delivery adapters stay under conversations or move to mesh after audit.
3. Whether `BadgeAggregator` lives under `gui/shell` or `gui/shared`.
4. Inbox row-building vs presenter ownership.
5. Conversations→ai inbound port vs one-way edge.

---

## Success criteria

- Vocabulary used consistently (delivery / conversations / call session / call media / **gui**).
- No new top-level `feature/chat`; no product UI layer named `src/ui/` (collides with `domain/ui`).
- Domain peers stay independent; `feature` stays Rml-free after f7; CI include checks green.
- SRC_LAYOUT + READMEs updated when folders actually rename/move.
