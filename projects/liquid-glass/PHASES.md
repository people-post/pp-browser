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
- [ ] Manual QA: compact Home, Sessions overlay, Me list

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
- [x] Remove multi-surface blur; frost via `ShellInterruption::CompactChromeFrostSurface`
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

## lg6 — Fallbacks, perf gates, agent docs

### Tasks

- [ ] **Reduce transparency** → `.surface-chrome--solid` on all chrome (disable frost)
- [ ] Optional: profile pref to disable frost tier entirely (all opaque)
- [ ] Complete test matrix from DESIGN.md
- [ ] Block merge if perf gate fails (LG005 device)

### Exit criteria

- Accessibility fallbacks manual-tested; perf documented in CURRENT_STATE.

---

## lg7 — Promote to stable docs

- [ ] Add **Compact floating chrome** section to `docs/ui/UI_DESIGN_SYSTEM.md`
- [ ] Update `docs/ui/WINDOW_SHELL.md` compact chrome section
- [ ] Update `docs/ui/RCSS_PROFILE.md` — chrome utilities theme-only
- [ ] Mark project README **Done**; freeze DECISIONS

---

## Agent batch delivery order

1. lg1 — done.
2. lg2–lg3 — done.
3. **lg8 — done** (this pivot).
4. **Next:** lg6 fallbacks → lg7 docs.
