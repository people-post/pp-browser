# Feature / app reorg — current state

**Updated:** 2026-09-03  
**Phase:** f0 done → **f1 next**

## What already shipped (prior work)

- `src/base/` retired → `src/foundation/` + `src/domain/`
- Domain peers strictly independent; cross-need via `common/` + feature wiring
- Feature module libs acyclic: `settings → ai → messaging → ui → chat`
- Ports + app bridges break UI↔functional cycles ([UI_FUNCTIONAL_BOUNDARY.md](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md))
- Include guard: [`scripts/check_feature_includes.sh`](../../scripts/check_feature_includes.sh)

## Snapshot (approx. non-test `.h`/`.cpp`)

| Area | Count | Notes |
|------|------:|-------|
| `feature/messaging` | ~127 | Hub ~2.2k LOC; owns call stack + some stores |
| `feature/ui` | ~105 | Shell + many screens + ~20 ports |
| `feature/chat` | ~20 | `ChatController` ~3k LOC |
| `feature/ai` | ~35 | Healthier |
| `feature/settings` | ~28 | Healthier; profile/security UI still partly in ui |
| `app` | ~28 | `Application::Initialize` ~850 LOC of wiring |

## Pain points (today)

1. Feature messaging holds domain-grade stores (`SqlitePskSessionStore`, `CallMediaKeyStore`) and pure helpers.
2. Calls have no module: SM under messaging, chrome under ui.
3. `feature/ui` is a grab-bag; cross-deps look worse than link order alone shows.
4. Inbox builds presentation rows inside messaging (`domain/ui/ChatWidgetTypes`).
5. messaging → ai hard-includes `AgentSession`.
6. ~28 ports; Application is the only complete wiring map.

## Next agent — start here

1. **f1:** move `SqlitePskSessionStore` (+ ideally `CallMediaKeyStore` in the same or follow-up PR) to **`domain/messaging/` flat** ([F006](DECISIONS.md#f006--sure-peels-use-existing-domain-peers-no-new-peers)).
2. Keep Hub ownership; update includes/CMake/tests only. **No new domain peers / no messaging subfolder migration in f1.**
3. Mark PHASES checkboxes.
4. Do **not** start `feature/calls` folder split until f1–f2 peels land (or an ADR explicitly overrides).

## Related docs still slightly stale

| Doc | Stale bit |
|-----|-----------|
| `src/feature/README.md` | Still mentions `base/` includes in places |
| `RUNTIME_COMPOSITION.md` | Diagrams still say `base/` |
| `docs/README.md` | SRC_LAYOUT blurb may still say foundation+domain under `base/` |

Fix those opportunistically when touching layout (prefer same PR as structural moves).
