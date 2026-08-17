# Shell chrome projection (surface → shell)

**Tier:** architecture  
**Related:** [WINDOW_SHELL.md](../ui/WINDOW_SHELL.md), [UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md)

## Pattern

```text
Presenter (feature)          App bridge (composition root)         ShellHost
─────────────────            ─────────────────────────────         ────────
surface state only           knows presenter + shell
push *SurfaceSnapshot      → Project + Classify → ShellChromeOp → DirtyNav / SyncLayout
```

- **Presenter** owns Rml surface models; pushes a **surface snapshot** (no chrome enums).
- **Bridge** (`*ShellBridge` in `app/`) maps snapshot → projection → `ShellChromeOp` → `ShellHost`.
- Pure map/classify stays in `feature/ui/*ChromeSync` (gtest-friendly).
- Shared apply: `ShellChromeApplyPorts` / `MakeShellChromeApplyPorts`.
- **Shell chrome ports are apply-only** — they must not return mutable references into `ShellHost::State()`. Presenters own local chrome snapshots and push apply ops.

## Done

| Surface | Snapshot | Bridge | Notes |
|---------|----------|--------|-------|
| Contacts | `ContactsSurfaceSnapshot` | `ContactsShellBridge` | `LastSurface()` feeds badge `contacts_unread` |
| Chat | `ChatSurfaceSnapshot` | `ChatShellBridge` | `ChatThreadChrome` notifies via callback |
| People picker | `PeoplePickerSurfaceSnapshot` | `PeoplePickerShellBridge` | overlay open/close |
| Call | `CallChromeSnapshot` (`ring_` / `in_call_` in `CallController`) | `ShellCallChromePorts::apply_snapshot` | Classify via `CallChromeSync`; `ShellHost::ApplyCallChromeSnapshot` copies then Remount/Dirty |
| PIN gate | `PinGateState` (`pin_state_` in `PinGateController`) | `ShellPinGatePorts::apply_pin_gate` | `dirty_pin_gate` + `remount_pin_gate`; bound pin inputs pulled via read-only snapshot before submit |

`dirty_nav_chrome` removed from `ShellNavigationPorts` — presenters must not shotgun-dirty nav.

Setup / deferred fonts use `set_fonts_ready` + read-only `fonts_ready()` (no `bool&` into `ShellState`).

## Follow-up

- Settings: usually own-model `DirtyAll` only; add bridge if shell projection appears
- Trim redundant `NotifySurfaceChanged` after toast-only paths (safe no-ops today)
