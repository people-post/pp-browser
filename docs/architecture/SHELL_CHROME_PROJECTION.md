# Shell chrome projection (surface → shell)

**Tier:** architecture  
**Related:** [WINDOW_SHELL.md](../ui/WINDOW_SHELL.md), [UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md)

## Pattern

```text
Presenter (feature)          App bridge (composition root)         ShellHost
─────────────────            ─────────────────────────────         ────────
surface state only           knows presenter + shell
push ContactsSurfaceSnapshot → Project + Classify → apply Dirty*/SyncLayout
```

- **Presenter** owns Rml surface models; pushes a **surface snapshot** (no `DirtyNav` / chrome enums).
- **Bridge** (`ContactsShellBridge` in `app/`) maps snapshot → `ContactsShellProjection` → `ContactsChromeUpdate` → `ShellHost`.
- Pure map/classify stays in `feature/ui/ContactsChromeSync` (gtest-friendly).

## Contacts (done)

| Piece | Layer |
|-------|--------|
| `ContactsSurfaceSnapshot` + notify ports | `feature/ui` |
| `ContactsChromeSync` (project/classify) | `feature/ui` |
| `ContactsShellBridge` | `app/` |
| `ShellContactsChromePorts` / `MakeShell…` | `feature/ui` (apply sink for bridge) |

`CallController` remains special-cased (writes `CallRingState` / `CallInProgressState` and classifies itself).

## Follow-up

| Surface | Still uses | Target |
|---------|------------|--------|
| `ChatController` / `ChatThreadChrome` | `dirty_nav_chrome` via `ShellDirty()` | Same push-snapshot + app bridge |
| `PeoplePickerController` | `ShellDirty()` → `dirty_nav_chrome` | Same when needed |
| `BadgeAggregator` | `contacts_unread` always 0 | Feed from last contacts snapshot in bridge / app |
