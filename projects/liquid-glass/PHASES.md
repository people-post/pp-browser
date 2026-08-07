# Floating Chrome — phases

Execute in order. Each phase should land reviewable PRs.

---

## lg0 — Project docs + ADRs

- [x] `projects/liquid-glass/` README, DESIGN, CURRENT_STATE, DECISIONS, PHASES
- [x] Register in `projects/README.md`

---

## lg1 — Layout foundation (floating compact chrome)

**Goal:** Content scrolls **under** bottom nav; validate overlap geometry and safe area.

- [x] Floating bottom chrome + content padding (see DESIGN.md)
- [x] Safe area plumbing
- [x] Interruption tests

---

## lg2 — Chrome materials (RCSS)

**Goal:** Theme tokens + utilities for compact chrome.

- [x] Tokens in `colors-*.rcss`
- [x] Utilities in `components.rcss` (renamed `.surface-chrome` in lg8)
- [x] Applied to compact shell surfaces

---

## lg3 — Floating shape + edge treatment

- [x] Nav pill geometry, active capsule, specular border
- [x] Overlay/transient headers, composer strip

---

## lg8 — Floating Chrome pivot (top-layer frost only) — **LG008**

**Goal:** Opaque chrome by default; at most one `backdrop-filter` per frame on top interruption chrome.

### Tasks

- [x] ADR LG008; update DESIGN / CURRENT_STATE / README
- [x] Rename `.surface-glass` → `.surface-chrome` (+ `--frost`, `--solid`)
- [x] Remove multi-surface blur; frost via `ShellInterruption::ResolveFrostSurface`
- [x] Wire frost classes in `ShellHost` serialization
- [x] Opaque auxiliary sheet body + top strip; opaque account sheet body
- [x] Restore standard scrim opacity (light theme)
- [x] Unit tests for frost surface selection
- [x] Cancel lg4 / lg5; close LG004 (no shader spike)

### Exit criteria

- Only one element has `backdrop-filter` at a time in compact shell.
- All non-frost chrome uses opaque fill.

---

## lg4 — Motion + scroll coupling — **Cancelled (LG008)**

---

## lg5 — Renderer extensions — **Cancelled (LG008)**

---

## lg6 — Fallbacks, perf gates, agent docs — **Done**

- [x] **Reduce transparency** → `.surface-chrome--solid` (`ProfilePreferences.reduce_transparency`, Me → Appearance)
- [x] **Frost tier** default on; dogfood off via `compact_chrome_frost` in profile JSON
- [x] Perf gate devices documented (LG005)
- [x] Resolve LG005–LG007

---

## lg7 — Promote to stable docs — **Done**

- [x] **Compact floating chrome** in `docs/ui/UI_DESIGN_SYSTEM.md`
- [x] `docs/ui/WINDOW_SHELL.md` compact chrome section
- [x] `docs/ui/RCSS_PROFILE.md` — chrome utilities theme-only
- [x] `docs/contracts/DATA_LAYOUT.md` + `docs/ops/CONFIGURATION.md` prefs
- [x] Project README **Done**

---

## Agent batch delivery order

1. lg1 — done.
2. lg2–lg3 — done.
3. **lg8 — done** (this pivot).
4. **Next:** lg6 fallbacks → lg7 docs.
