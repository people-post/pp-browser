# Working North Star — feature / app

**Status:** end-state names locked in [F007](DECISIONS.md#f007--vocabulary--end-state-feature-names); paths migrate in phases  
**Promote** folder renames into [SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md) when they ship.

Layer north star from SRC_LAYOUT still holds:

> `common` names the shared language; `foundation` implements the shared kernel; `domain` implements independent product capabilities; `feature` composes them; `app` constructs the graph.

This file refines **feature** and **app**.

---

## Vocabulary ([F007](DECISIONS.md#f007--vocabulary--end-state-feature-names))

| Term | Means |
|------|--------|
| **Delivery** | Envelope/stream send–receive |
| **Conversations** | Threads, sync, attachments, groups — product hub |
| **Call session** | Ring → accept → keys → topology → leave |
| **Call media** | A/V frames, codecs, legs |
| **Chat** | User-facing screen label only — **not** a top-level feature folder |

**conversations ∥ call session** → both use **delivery**; call session drives **call media**.

---

## Domain peer set ([F006](DECISIONS.md#f006--sure-peels-use-existing-domain-peers-no-new-peers))

| Peer | Role |
|------|------|
| `messaging` | Conversation/call **record & codec** engines (keep name; not the product hub) |
| `people` | Contacts/identity helpers |
| `mesh` | Amp host, reachability, L4 call-media / media-relay |
| `media` | Call-media capture/playback/codecs only |
| `ui` | Pure presentation policy (no Rml controllers) |
| `net` / `ai` | Unchanged |

Do **not** add `domain/calls`.

---

## Litmus

| Layer | Put it here when… | Not when… |
|-------|-------------------|-----------|
| **domain** | Owns one store, codec, client, or pure policy | Coordinates several peers or owns UI lifecycle |
| **feature** | Coordinates workflows or screens | Single-purpose engine another feature would want alone |
| **app** | Lifetimes + cross-controller wiring | Product policy or I/O engines |
| **common** | Second domain peer must compile against the name | Only one peer needs it |

**Cross-peer rule:** two of `{people, messaging, net, mesh, media, ui}` → feature or `common` contracts — never domain→domain.

---

## End-state feature map ([F007](DECISIONS.md#f007--vocabulary--end-state-feature-names))

```
feature/
  settings/        # config apply + section handlers
  ai/              # agent session, turn pipeline, tools, bindings
  conversations/   # TODAY: feature/messaging — hub, delivery, sync, groups, façade
  calls/           # call session — f4v1 nested as messaging/calls/; later top-level
  shell/           # ShellHost, gestures, feedback, RmlMount, chrome sync (from ui)
  contacts/        # Contacts + PeoplePicker (from ui)
  ui/              # shared ports + presenters; ABSORBS today’s feature/chat (ChatController)
                   # NO top-level feature/chat/
```

### Legacy → end-state

| Today | End state | Notes |
|-------|-----------|-------|
| `feature/messaging` | `feature/conversations` | Rename when hub ownership is clean |
| `feature/messaging` Call\* | `feature/calls` | [F004](DECISIONS.md#f004--calls-home-nested-band-first-then-top-level): nest first |
| `feature/chat` | fold into `feature/ui` | Presenter stays; folder name drops |
| `feature/ui` | `shell` + `contacts` + residual `ui` | f5 |

Intended link order (end state):

```
settings → ai → conversations → calls → shell / contacts / ui
```

(`chat` is not a link node.)

Ownership: **app** owns `ConversationsHub` and `CallStack` separately; conversations does **not** own call session long-term.

---

## Working app map

- Keep **ownership** in `Application` (composition root) ([F003](DECISIONS.md#f003--app-stays-composition-root)).
- Prefer **named wirers** (`WireShell`, `WireConversations`, `WireCalls`, …).
- Keep thin `*Bridge` helpers for cycle breaks / chrome apply.

---

## Still open

1. Exact f5 split names (`shell` vs `ui/shell`).
2. Whether Amp *chat* delivery adapters stay under conversations or move to mesh after audit.
3. Whether `BadgeAggregator` lives with shell or contacts.
4. Inbox row-building vs presenter ownership.
5. Conversations→ai inbound port vs one-way edge.

---

## Success criteria

- Vocabulary used consistently in new docs/PRs (delivery / conversations / call session / call media).
- No new top-level `feature/chat` dependency in the plan.
- Domain peers stay independent; CI include checks green.
- SRC_LAYOUT + `src/feature/README.md` updated when folders actually rename/move.
