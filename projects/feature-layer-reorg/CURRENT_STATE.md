# Feature / app reorg — current state

**Updated:** 2026-09-03  
**Phase:** f3 landed; **F007 names locked** → **f4v1 nested calls band next**

## What already shipped

- `src/base/` retired → `src/foundation/` + `src/domain/`
- Domain peers strictly independent; cross-need via `common/` + feature wiring
- Feature module libs acyclic (today): `settings → ai → messaging → ui → chat`
- **f0–f3** peels + docs; **F004/F006/F007** naming & peer rules

## Locked naming ([F007](DECISIONS.md#f007--vocabulary--end-state-feature-names))

| End-state folder | Today | Role |
|------------------|-------|------|
| `conversations/` | `feature/messaging` | Conversations hub + delivery |
| `calls/` | Call\* under messaging | Call **session** (nest first per F004) |
| `ui/` (+ shell/contacts) | `feature/ui` + **`feature/chat`** | Presenters; **no top-level chat/** |
| `domain/messaging` | same | Record/codec engines (keep name) |

Vocabulary: **delivery** / **conversations** / **call session** / **call media**; “chat” = UI copy only.

## Snapshot (paths today)

| Area | Notes |
|------|-------|
| `feature/messaging` | Legacy path for conversations + still owns call session |
| `feature/chat` | Legacy top-level screen module — to fold into ui |
| `feature/ui` | Grab-bag + presenters |
| `domain/messaging` | Stores/codecs + history responder + PSK/call-key peels |

## Next agent — start here

1. **f4v1:** nest `feature/messaging/calls/` (same CMake target); move Call\* only.
2. Do **not** add top-level `pp_feature_calls` until the Hub↔MeshMessagingService cycle is broken.
3. Do **not** introduce a new top-level `feature/chat` in any plan — absorb into ui/shell (f5).

## Related docs still slightly stale

| Doc | Stale bit |
|-----|-----------|
| `RUNTIME_COMPOSITION.md` | May still say `base/`; diagrams use MessagingHub |
| `SRC_LAYOUT` feature table | Still lists `feature/chat` as current — update when f5 ships |

Opportunistic fixes OK; F007 is the planning source of truth until paths move.
