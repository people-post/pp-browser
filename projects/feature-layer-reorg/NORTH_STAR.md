# Working North Star — feature / app

**Status:** working hypothesis (revise freely; promote slices into [SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md) when proven)  
**Not normative** until an ADR marks a slice locked.

Layer north star from SRC_LAYOUT still holds:

> `common` names the shared language; `foundation` implements the shared kernel; `domain` implements independent product capabilities; `feature` composes them; `app` constructs the graph.

This file only refines **feature** and **app**.

---

## Litmus (stable enough to use now)

| Layer | Put it here when… | Not when… |
|-------|-------------------|-----------|
| **domain** | Owns one store, codec, client, or pure policy; reusable without a screen | It coordinates several peers or owns UI lifecycle |
| **feature** | Coordinates multiple domain/foundation modules into a workflow or screen | It is a single-purpose engine another feature would want alone |
| **app** | Owns lifetimes and cross-controller / SessionStore wiring | It contains product policy or I/O engines |
| **common** | A second domain peer must compile against the name without linking the owner | Only one peer needs it |

**Cross-peer rule:** if a type needs two of `{people, messaging, net, mesh, media, ui}`, it stays in **feature** (or peels via **common** contracts) — never create a domain→domain edge.

---

## Working feature map (hypothesis)

```
feature/
  settings/     # config apply + settings section handlers (incl. profile/security UI sections eventually)
  ai/           # agent session, turn pipeline, tools, bindings
  messaging/    # hub, sync, groups, relay pipeline, façade, Amp *chat* adapters
  calls/        # (candidate) CallStack, CSM, lifecycle, topology, CallUiBackend, call ports
  shell/        # (candidate) ShellHost, gestures, feedback, RmlMount, DocumentLoader, chrome sync
  contacts/     # (candidate) ContactsController, PeoplePicker*, related ports/chrome
  ui/           # residual shared presenters/ports OR fold into shell over time
  chat/         # chat screen only; shrink ChatController surface
```

Intended link order (if/when splits land):

```
settings → ai → messaging → calls → shell / contacts / ui → chat
```

### Mental model (names today vs intent)

| Today | Intent |
|-------|--------|
| `feature/messaging` | Connectivity **product hub** — not a twin of `domain/messaging` |
| `domain/messaging` | Stores, codecs, validators, call types/session store |
| `feature/chat` | Chat **screen** |
| `feature/ui` | Temporary grab-bag: shell + many screens — to be split when peels settle |
| Call\* | Domain: media engine + codecs; feature/messaging (today): CSM/lifecycle; feature/ui: chrome |

---

## Working app map

- Keep **ownership** in `Application` (composition root).
- Prefer **named wirers** over one monolithic `Initialize` (`WireShell`, `WireMessaging`, `WireCalls`, …).
- Keep thin `*Bridge` helpers for cycle breaks / chrome apply.
- Do not invent a presenter registry framework unless a wirer split proves insufficient.

---

## Open until peels teach us

Record answers in [DECISIONS.md](DECISIONS.md) when evidence lands; until then leave open:

1. Exact folder names (`shell` vs `ui/shell`, `calls` vs `messaging/calls`).
2. Whether Amp chat adapters move to `domain/mesh` or stay feature adapters.
3. Whether `BadgeAggregator` lives with shell or contacts.
4. How far to push Inbox row-building out of messaging (presenter ownership).
5. Whether messaging→ai becomes an inbound port in app, or stays a one-way feature edge.

---

## Success criteria (for this project)

- Feature messaging file count and hub include surface drop after peels.
- UI no longer includes messaging **engines** (only ports/façades) for reachability/call policy once those peels land.
- Domain peers remain independent; CI include/public-lib checks stay green.
- SRC_LAYOUT + `src/feature/README.md` match reality after each structural phase.
