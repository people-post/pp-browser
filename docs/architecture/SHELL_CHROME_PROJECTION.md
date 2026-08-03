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

## Done

| Surface | Snapshot | Bridge | Notes |
|---------|----------|--------|-------|
| Contacts | `ContactsSurfaceSnapshot` | `ContactsShellBridge` | `LastSurface()` feeds badge `contacts_unread` |
| Chat | `ChatSurfaceSnapshot` | `ChatShellBridge` | `ChatThreadChrome` notifies via callback |
| People picker | `PeoplePickerSurfaceSnapshot` | `PeoplePickerShellBridge` | overlay open/close |
| Call | — | — | **Special-cased** (`CallController` + `CallChromeSync`) |

`dirty_nav_chrome` removed from `ShellNavigationPorts` — presenters must not shotgun-dirty nav.

## Follow-up

- Settings: usually own-model `DirtyAll` only; add bridge if shell projection appears
- Pin gate: already domain `dirty_pin_gate`
- Trim redundant `NotifySurfaceChanged` after toast-only paths (safe no-ops today)
