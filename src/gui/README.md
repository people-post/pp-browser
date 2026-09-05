# `src/gui`

Product **GUI** layer for pp-browser: Rml/SDL presenters, shell chrome, and screen controllers.

```
app → gui → feature → domain → foundation → common → pp_common
gui    ← you are here
```

Named **`gui`** (not `ui`) so it does not collide with domain peer `domain/ui` (non-Rml presentation policy). See [F008](../../projects/feature-layer-reorg/DECISIONS.md#f008--gui-layer-above-feature) and [UI_FUNCTIONAL_BOUNDARY.md](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md).

**Rule:** dependencies flow downward only. GUI may use `feature/`, `domain/`, `foundation/`, `common/`, and `lib/`; it must not `#include` from `app/`. Feature must not `#include` from `gui/`.

Aggregate library: **`pp_gui`**. Includes use the repo root: `#include "gui/shell/ShellHost.h"`.

## Module map

```
src/gui/
├── shell/       ShellHost, mount, gestures, shell ports
├── contacts/    Contacts + people-picker
├── chat/        ChatController + screen helpers
├── *.cpp/h      Shared presenters (settings, call, pin, emoji, badges, flow, …)
└── tests/
```

Bands share one CMake target for now; split later only if the include graph warrants.

**Testing:** sparse unit/chrome checks; not the default correctness vehicle. Prefer feature/domain coverage; reserve GUI E2E for rare `B-UI`-style checks. Doctrine: [TESTING.md](../../docs/architecture/TESTING.md).
