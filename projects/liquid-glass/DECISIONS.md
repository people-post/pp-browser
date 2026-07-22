# Liquid Glass — decisions

Record dated outcomes. When shipped, mark **superseded by** stable doc — do not duplicate normative tables in two places.

---

## LG001 — Compact-only scope

**Date:** 2026-07-19  
**Status:** Accepted  
**Decision:** Liquid Glass applies only when `layout_mode == compact` (&lt;768dp). Expanded desktop shell keeps current flat productivity surfaces.  
**Rationale:** Apple Liquid Glass is a mobile idiom; expanded layout users expect dense pane separation (Notion/Slack). Reduces blast radius and GPU cost on desktop multi-pane.  
**Superseded by:** (pending) `docs/ui/UI_DESIGN_SYSTEM.md` Materials section

---

## LG002 — RmlUi backdrop-filter first; no native UIVisualEffectView in v1

**Date:** 2026-07-19  
**Status:** Accepted  
**Decision:** Implement glass via RCSS `backdrop-filter` and GL3/GLES renderer path shared with Android/desktop. Do not embed native iOS `UIVisualEffectView` or Android-only blur overlays in v1.  
**Rationale:** Single RML tree, one QA matrix, matches existing render stack. Native materials can be revisited if GLES path cannot reach fidelity on Apple Silicon iOS devices.  
**Superseded by:** —

---

## LG003 — Chrome surface inventory

**Date:** 2026-07-21  
**Status:** Superseded by [LG008](#lg008--floating-chrome-top-layer-frost-only) (sheet body opaque; frost on header strip only when top)  
**Decision:** **C (max)** — bottom nav pill + chat/transient headers + Home/overlay composer strip + **full auxiliary sheet glass body** + lighter scrim tint.  
**Rationale:** Highest material coverage for compact chrome; sheet body glass is acceptable GPU cost while the sheet is open. Visual QA / blur radius tuning deferred.  
**Superseded by:** (pending) `docs/ui/UI_DESIGN_SYSTEM.md` Materials section

**Options considered:**

| Option | Surfaces |
|--------|----------|
| A (minimal) | Bottom nav pill + chat/transient headers only |
| B | A + auxiliary sheet header strip + Home composer strip |
| **C (chosen)** | B + full sheet glass body + scrim tint adjustment |

---

## LG004 — Renderer fidelity path

**Date:** 2026-07-19  
**Status:** **Closed — skip lg5 (LG008, 2026-07-22)**  
**Decision:** Floating Chrome uses stock `backdrop-filter` on at most one surface. No custom shader spike.

---

## LG005 — Performance reference device

**Date:** 2026-07-19  
**Status:** Accepted (2026-07-22)  
**Decision:** Manual profiling on **Pixel 6a class** Android and **iPhone 15 Simulator**. Gate: compact chat scroll avg frame time with single-surface frost ≤ +2ms vs all-opaque on reference Android; ≤ 1 `backdrop-filter` surface visible.  
**Superseded by:** [CURRENT_STATE.md](CURRENT_STATE.md) perf gate table; [UI_DESIGN_SYSTEM.md](../../docs/ui/UI_DESIGN_SYSTEM.md#compact-floating-chrome-materials)

---

## LG006 — Reduce transparency setting

**Date:** 2026-07-19  
**Status:** Accepted (2026-07-22) — **Option A**  
**Decision:** Me → Appearance → **Reduce transparency** toggle (`ProfilePreferences.reduce_transparency`, schema v8). Applies `.surface-chrome--solid` and disables frost tier. System bridge deferred.  
**Superseded by:** [UI_DESIGN_SYSTEM.md](../../docs/ui/UI_DESIGN_SYSTEM.md#compact-floating-chrome-materials)

---

## LG007 — Feature flag default

**Date:** 2026-07-19  
**Status:** Accepted (2026-07-22)  
**Decision:** Frost tier **on** by default (`compact_chrome_frost = true`). Dogfood off via profile JSON only (not exposed in Settings v1). `reduce_transparency` disables frost for accessibility.  
**Superseded by:** [UI_DESIGN_SYSTEM.md](../../docs/ui/UI_DESIGN_SYSTEM.md#compact-floating-chrome-materials)

---

## LG008 — Floating Chrome: top-layer frost only

**Date:** 2026-07-22  
**Status:** Accepted  
**Decision:** Pivot from Apple-faithful Liquid Glass to **Floating Chrome**. All compact chrome uses opaque `.surface-chrome` by default. **At most one** visible chrome bar per frame may add `.surface-chrome--frost` (`backdrop-filter: blur(12px)`), selected by `ShellInterruption::ResolveFrostSurface` from the interruption stack. Cancel lg4 (scroll motion) and lg5 (custom shaders). Auxiliary sheet body is opaque; frost limited to top strip / headers.  
**Rationale:** Multi-surface blur caused O(N) backdrop passes during chat scroll; floating layout already delivers hierarchy. Top-layer-only frost preserves depth cue at bounded GPU cost.  
**Supersedes:** Apple-faithful targets in DESIGN.md; LG003=C full sheet glass; LG004 shader path; lg4/lg5 phases.
